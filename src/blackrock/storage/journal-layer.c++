// Sandstorm Blackrock
// Copyright (c) 2015 Sandstorm Development Group, Inc.
// All Rights Reserved

#include "journal-layer.h"
#include <kj/debug.h>
#include <unistd.h>
#include <kj/function.h>

namespace blackrock {
namespace storage {

JournalLayer::Object::Object(
    JournalLayer& journal, ObjectId id, kj::Own<BlobLayer::Object>&& innerParam)
    : journal(journal), id(id), inner(kj::mv(innerParam)), cachedXattr(inner->getXattr()) {
  KJ_REQUIRE(journal.openObjects.insert(std::make_pair(id, this)).second);
}
JournalLayer::Object::Object(
    JournalLayer& journal, ObjectId id, const Xattr& xattr, BlobLayer::Content& content)
    : journal(journal), id(id), cachedXattr(xattr), cachedContent(content) {
  KJ_REQUIRE(journal.openObjects.insert(std::make_pair(id, this)).second);
}
JournalLayer::Object::~Object() noexcept(false) {
  journal.openObjects.erase(id);
}

BlobLayer::Content& JournalLayer::Object::getContent() {
  KJ_IF_MAYBE(r, cachedContent) {
    return *r;
  } else {
    return inner->getContent();
  }
}

void JournalLayer::Object::update(
    Xattr newXattr, kj::Maybe<BlobLayer::Content&> newContent, uint changeCount) {
  // Called when a transaction is committed to the journal (but possibly before the journaled
  // operations have actually been written out to their final locations) to tell this object
  // what values to return from the getters.

  generation += changeCount;
  cachedXattr = newXattr;
  if (newContent != nullptr) {
    cachedContent = newContent;
  }
}

JournalLayer::RecoverableTemporary::RecoverableTemporary(
    JournalLayer& journal, RecoveryId id, kj::Own<BlobLayer::Temporary>&& inner)
    : journal(journal), id(id), inner(kj::mv(inner)), cachedXattr(inner->getXattr()) {}
JournalLayer::RecoverableTemporary::RecoverableTemporary(
    JournalLayer& journal, RecoveryId id, const TemporaryXattr& xattr, BlobLayer::Content& content)
    : journal(journal), id(id), cachedXattr(xattr), cachedContent(content) {}
JournalLayer::RecoverableTemporary::~RecoverableTemporary() noexcept(false) {}

BlobLayer::Content& JournalLayer::RecoverableTemporary::getContent() {
  KJ_IF_MAYBE(r, cachedContent) {
    return *r;
  } else {
    return inner->getContent();
  }
}

void JournalLayer::RecoverableTemporary::update(
    TemporaryXattr newXattr, kj::Maybe<BlobLayer::Content&> newContent, uint changeCount) {
  // Called when a transaction is committed to the journal (but possibly before the journaled
  // operations have actually been written out to their final locations) to tell this object
  // what values to return from the getters.

  generation += changeCount;
  cachedXattr = newXattr;
  if (newContent != nullptr) {
    cachedContent = newContent;
  }
}

// =======================================================================================

class JournalLayer::Transaction::LockedObject final
    : public BlobLayer::Object, public kj::Refcounted {
public:
  explicit LockedObject(kj::Own<JournalLayer::Object> objectParam)
      : object(kj::mv(objectParam)), created(false) {
    if (object->locked) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "transaction aborted due to conflict"));
    }
    object->locked = true;
  }

  LockedObject(kj::Own<JournalLayer::Object> objectParam,
               kj::Own<BlobLayer::Temporary> initialContent)
      : object(kj::mv(objectParam)), created(true),
        newContent(kj::mv(initialContent)) {
    if (object->locked) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "transaction aborted due to conflict"));
    }
    object->locked = true;
  }

  ~LockedObject() noexcept(false) {
    object->locked = false;
  }

  kj::Maybe<JournalEntry> getJournalEntry(uint64_t stagingId) {
    // Write a journal entry for this object's changes. If a temporary needs to be staged, do so
    // and assign it `stagingId`.

    if (changeCount == 0 || (created && removed)) return nullptr;

    JournalEntry entry;
    memset(&entry, 0, sizeof(entry));

    KJ_IF_MAYBE(c, newContent) {
      c->get()->setRecoveryId(RecoveryId(RecoveryType::STAGING, stagingId));
      entry.stagingId = stagingId;
    }

    entry.object.id = object->id;
    entry.object.xattr = getXattr();

    if (created) {
      entry.type = JournalEntry::Type::CREATE_OBJECT;
    } else if (removed) {
      entry.type = JournalEntry::Type::DELETE_OBJECT;
    } else if (newContent == nullptr) {
      entry.type = JournalEntry::Type::UPDATE_XATTR;
    } else {
      entry.type = JournalEntry::Type::UPDATE_OBJECT;
    }

    return entry;
  }

  kj::Function<void(BlobLayer& blobLayer)> commit() {
    // Commit to the changes made to this object. That is:
    // 1. Update the journal-layer object to reflect these changes.
    // 2. Return a function which should be called later to actually execute the changes. This
    //    function will be called once the journal entry for this change has been synced to disk.
    //
    // No other methods of LockedObject will be called after commit(), and the LockedObject will
    // be destroyed before the returned callback is called, therefore the callback should take
    // ownership of anything it needs.

    if (changeCount == 0 || (created && removed)) return [](BlobLayer& blobLayer) {};

    object->update(getXattr(),
        newContent.map([](auto& t) -> BlobLayer::Content& { return t->getContent(); }),
        changeCount);

    Xattr xattr = getXattr();

    if (created) {
      auto content = kj::mv(KJ_ASSERT_NONNULL(newContent));
      return [KJ_MVCAP(object),KJ_MVCAP(content),xattr](BlobLayer& blobLayer) mutable {
        object->inner = blobLayer.createObject(object->id, xattr, kj::mv(content));
      };
    } else if (removed) {
      return [KJ_MVCAP(object)](BlobLayer& blobLayer) mutable {
        object->inner->remove();
      };
    } else KJ_IF_MAYBE(c, newContent) {
      auto content = kj::mv(*c);
      return [KJ_MVCAP(object),KJ_MVCAP(content),xattr](BlobLayer& blobLayer) mutable {
        object->inner->overwrite(xattr, kj::mv(content));
      };
    } else {
      return [KJ_MVCAP(object),xattr](BlobLayer& blobLayer) mutable {
        object->inner->setXattr(xattr);
      };
    }
  }

  void overwrite(Xattr xattr, kj::Own<BlobLayer::Temporary> content) override {
    ++changeCount;
    newXattr = xattr;
    newContent = kj::mv(content);
  }

  Xattr getXattr() override {
    KJ_IF_MAYBE(x, newXattr) {
      return *x;
    } else {
      return object->getXattr();
    }
  }

  void setXattr(Xattr xattr) override {
    ++changeCount;
    newXattr = xattr;
  }

  void remove() override {
    ++changeCount;
    removed = true;
  }

  uint64_t getGeneration() override {
    return object->getGeneration() + changeCount;
  }

  BlobLayer::Content& getContent() override {
    KJ_IF_MAYBE(c, newContent) {
      return c->get()->getContent();
    } else {
      return object->getContent();
    }
  }

private:
  kj::Own<JournalLayer::Object> object;

  uint changeCount = 0;
  bool created;
  bool removed = false;
  kj::Maybe<Xattr> newXattr;
  kj::Maybe<kj::Own<BlobLayer::Temporary>> newContent;
};

class JournalLayer::Transaction::LockedTemporary final
    : public BlobLayer::Temporary, public kj::Refcounted {
public:
  explicit LockedTemporary(kj::Own<JournalLayer::RecoverableTemporary> objectParam)
      : object(kj::mv(objectParam)), created(false) {
    if (object->locked) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "transaction aborted due to conflict"));
    }
    object->locked = true;
  }

  LockedTemporary(kj::Own<JournalLayer::RecoverableTemporary> objectParam,
                  kj::Own<BlobLayer::Temporary> initialContent)
      : object(kj::mv(objectParam)), created(true), newContent(kj::mv(initialContent)) {
    if (object->locked) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "transaction aborted due to conflict"));
    }
    object->locked = true;
  }

  ~LockedTemporary() noexcept(false) {
    object->locked = false;
  }

  void remove() {
    ++changeCount;
    removed = true;
  }

  kj::Maybe<JournalEntry> getJournalEntry(uint64_t stagingId) {
    // Write a journal entry for this object's changes. If a temporary needs to be staged, do so
    // and assign it `stagingId`.

    if (changeCount == 0 || (created && removed)) return nullptr;

    JournalEntry entry;
    memset(&entry, 0, sizeof(entry));

    KJ_IF_MAYBE(c, newContent) {
      c->get()->setRecoveryId(RecoveryId(RecoveryType::STAGING, stagingId));
      entry.stagingId = stagingId;
    }

    entry.temporary.id = object->id;
    entry.temporary.xattr = getXattr();

    if (created) {
      entry.type = JournalEntry::Type::CREATE_TEMPORARY;
    } else if (removed) {
      entry.type = JournalEntry::Type::DELETE_TEMPORARY;
    } else if (newContent == nullptr) {
      entry.type = JournalEntry::Type::UPDATE_TEMPORARY_XATTR;
    } else {
      entry.type = JournalEntry::Type::UPDATE_TEMPORARY;
    }

    return entry;
  }

  kj::Function<void(BlobLayer&)> commit() {
    // Commit to the changes made to this object. That is:
    // 1. Update the journal-layer object to reflect these changes.
    // 2. Return a function which should be called later to actually execute the changes. This
    //    function will be called once the journal entry for this change has been synced to disk.
    //
    // No other methods of LockedObject will be called after commit(), and the LockedObject will
    // be destroyed before the returned callback is called, therefore the callback should take
    // ownership of anything it needs.

    if (changeCount == 0 || (created && removed)) return [](BlobLayer& blobLayer) {};

    object->update(getXattr(),
        newContent.map([](auto& t) -> BlobLayer::Content& { return t->getContent(); }),
        changeCount);

    TemporaryXattr xattr = getXattr();

    if (created) {
      auto content = kj::mv(KJ_ASSERT_NONNULL(newContent));
      return [KJ_MVCAP(object),KJ_MVCAP(content),xattr](BlobLayer& blobLayer) mutable {
        content->setRecoveryId(object->id, xattr);
        object->inner = kj::mv(content);
      };
    } else if (removed) {
      return [KJ_MVCAP(object)](BlobLayer& blobLayer) mutable {
        // Nothing to do here: just release the temporary.
      };
    } else KJ_IF_MAYBE(c, newContent) {
      auto content = kj::mv(*c);
      return [KJ_MVCAP(object),KJ_MVCAP(content),xattr](BlobLayer& blobLayer) mutable {
        object->inner->overwrite(xattr, kj::mv(content));
      };
    } else {
      return [KJ_MVCAP(object),xattr](BlobLayer& blobLayer) mutable {
        object->inner->setXattr(xattr);
      };
    }
  }

  void setRecoveryId(RecoveryId id) override {
    KJ_UNIMPLEMENTED("please use Transaction::createRecoverableTemporary");
  }

  void setRecoveryId(RecoveryId id, TemporaryXattr xattr) override {
    KJ_UNIMPLEMENTED("please use Transaction::createRecoverableTemporary");
  }

  void overwrite(TemporaryXattr xattr, kj::Own<Temporary> replacement) override {
    ++changeCount;
    newXattr = xattr;
    newContent = kj::mv(replacement);
  }

  TemporaryXattr getXattr() override {
    KJ_IF_MAYBE(x, newXattr) {
      return *x;
    } else {
      return object->getXattr();
    }
  }

  void setXattr(TemporaryXattr xattr) override {
    ++changeCount;
    newXattr = xattr;
  }

  BlobLayer::Content& getContent() override {
    KJ_IF_MAYBE(c, newContent) {
      return c->get()->getContent();
    } else {
      return object->getContent();
    }
  }

private:
  kj::Own<JournalLayer::RecoverableTemporary> object;

  uint changeCount = 0;
  bool created;
  bool removed = false;
  kj::Maybe<TemporaryXattr> newXattr;
  kj::Maybe<kj::Own<BlobLayer::Temporary>> newContent;
};

// =======================================================================================

JournalLayer::Transaction::Transaction(JournalLayer &journal): journal(journal) {}
JournalLayer::Transaction::~Transaction() noexcept(false) {}

kj::Own<BlobLayer::Object> JournalLayer::Transaction::wrap(Object& object) {
  auto result = kj::refcounted<LockedObject>(kj::addRef(object));
  objects.add(kj::addRef(*result));
  return kj::mv(result);
}

kj::Own<BlobLayer::Temporary> JournalLayer::Transaction::wrap(RecoverableTemporary& object) {
  auto result = kj::refcounted<LockedTemporary>(kj::addRef(object));
  temporaries.add(kj::addRef(*result));
  return kj::mv(result);
}

kj::Own<JournalLayer::Object> JournalLayer::Transaction::createObject(
    ObjectId id, Xattr xattr, kj::Own<BlobLayer::Temporary> content) {
  auto result = kj::refcounted<JournalLayer::Object>(
      journal, id, xattr, content->getContent());
  objects.add(kj::refcounted<LockedObject>(kj::addRef(*result), kj::mv(content)));
  return kj::mv(result);
}

kj::Own<JournalLayer::RecoverableTemporary> JournalLayer::Transaction::createRecoverableTemporary(
    RecoveryId id, TemporaryXattr xattr, kj::Own<BlobLayer::Temporary> content) {
  auto result = kj::refcounted<JournalLayer::RecoverableTemporary>(
      journal, id, xattr, content->getContent());
  temporaries.add(kj::refcounted<LockedTemporary>(kj::addRef(*result), kj::mv(content)));
  return kj::mv(result);
}

kj::Promise<void> JournalLayer::Transaction::commit(
    kj::Maybe<kj::Own<RecoverableTemporary>> tempToConsume) {
  kj::Promise<void> result = nullptr;

  KJ_IF_MAYBE(t, tempToConsume) {
    auto wrapper = kj::refcounted<LockedTemporary>(kj::mv(*t));
    wrapper->remove();
    temporaries.add(kj::mv(wrapper));
  }

  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    kj::Vector<JournalEntry> entries(objects.size() + temporaries.size());
    kj::Vector<kj::Function<void(BlobLayer&)>> executeCallbacks(
        objects.size() + temporaries.size());

    // Build the transaction.
    for (auto& object: objects) {
      KJ_IF_MAYBE(entry, object->getJournalEntry(journal.stagingIdCounter++)) {
        entries.add(*entry);
      }
      executeCallbacks.add(object->commit());
    }
    for (auto& temp: temporaries) {
      KJ_IF_MAYBE(entry, temp->getJournalEntry(journal.stagingIdCounter++)) {
        entries.add(*entry);
      }
      executeCallbacks.add(temp->commit());
    }

    // Write to the journal.
    auto& journalContent = journal.journalFile->getContent();
    journalContent.write(journal.journalPosition, entries.asPtr().asBytes());
    uint64_t oldPosition = journal.journalPosition;
    uint64_t newPosition = oldPosition + entries.asPtr().asBytes().size();
    journal.journalPosition = newPosition;
    auto& journalRef = journal;

    // Sync the journal. As soon as this is done, we can safely return success to the caller.
    auto fork = journalContent.sync().fork();
    result = fork.addBranch();

    // Sequence the actual execution of this transaction into the write queue.
    auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
    promises.add(fork.addBranch());
    promises.add(kj::mv(journal.writeQueue));
    journal.writeQueue = kj::joinPromises(promises.finish())
        .then([KJ_MVCAP(executeCallbacks),&journalRef]() mutable {
      for (auto& callback: executeCallbacks) {
        callback(*journalRef.blobLayer);
      }

      // We have to sync() to make sure all the effects of the transaction have hit disk.
      // TODO(now): Offload sync to another thread. It doesn't even have to sync frequently; every
      //   30 seconds would be fine.
      sync();
    }).then([oldPosition,newPosition,&journalRef]() mutable {
      // We can now safely punch out our journal entry, as it has been completely synced to disk.

      // Round down to nearest block, since holes can only be punched at block boundaries. It's
      // OK if some of a journal entry gets left around for a while.
      oldPosition &= ~(BLOCK_SIZE - 1);
      newPosition &= ~(BLOCK_SIZE - 1);
      uint64_t delta = newPosition - oldPosition;

      // Punch dat hole.
      if (delta > 0) {
        journalRef.journalFile->getContent().zero(oldPosition, delta);
      }
    }, [](kj::Exception&& exception) {
      // It would appear that we failed to actually execute the transaction after writing it to
      // the journal and confirming commit to the client. We should abort now and hope that things
      // get fixed up during recovery.
      KJ_DEFER(abort());
      KJ_LOG(FATAL, "exception during journal execution; aborting", exception);
    });
  })) {
    KJ_DEFER(abort());
    KJ_LOG(FATAL, "exception during journal commit; aborting", *exception);
  }

  return result;
}

// =======================================================================================

kj::Own<JournalLayer::RecoverableTemporary>
JournalLayer::RecoveredTemporary::keepAs(RecoveryId newId) {
  return kj::refcounted<RecoverableTemporary>(journal, newId, inner->keepAs(newId, xattr));
}

void JournalLayer::RecoveredTemporary::setXattr(TemporaryXattr xattr) {
  this->xattr = xattr;
}

void JournalLayer::RecoveredTemporary::overwrite(
    TemporaryXattr xattr, kj::Own<BlobLayer::RecoveredTemporary> replacement) {
  this->xattr = xattr;
  inner = kj::mv(replacement);
}

JournalLayer::RecoveredTemporary::RecoveredTemporary(
    JournalLayer& journal, kj::Own<BlobLayer::RecoveredTemporary> inner)
    : journal(journal), oldId(inner->getOldId()), xattr(inner->getTemporaryXattr()),
      inner(kj::mv(inner)) {}

JournalLayer::RecoveredTemporary::RecoveredTemporary(
    JournalLayer& journal, RecoveryId oldId, TemporaryXattr xattr,
    kj::Own<BlobLayer::RecoveredTemporary> inner)
    : journal(journal), oldId(oldId), xattr(xattr), inner(kj::mv(inner)) {}

// =======================================================================================

kj::Promise<kj::Maybe<kj::Own<JournalLayer::Object>>> JournalLayer::openObject(ObjectId id) {
  // Check the openObjects map to see if this object is already open. Note that the caller is
  // not allowed to call openObject() again without having first dropped the original reference.
  // However, it is possible that the caller has in fact dropped the reference, but that the
  // object is being held open because it is still part of a transaction that has not yet
  // completed. This is why the map lookup is needed here.
  auto iter = openObjects.find(id);
  if (iter != openObjects.end()) {
    return kj::Maybe<kj::Own<JournalLayer::Object>>(kj::addRef(*iter->second));
  }

  return blobLayer->openObject(id).then([this,id](auto&& maybeObject) {
    return kj::mv(maybeObject).map([this,id](kj::Own<BlobLayer::Object>&& object) {
      JournalLayer& super = *this;
      return kj::refcounted<JournalLayer::Object>(super, id, kj::mv(object));
    });
  });
}

kj::Own<BlobLayer::Temporary> JournalLayer::newDetachedTemporary() {
  return blobLayer->newTemporary();
}

// =======================================================================================

JournalLayer::Recovery::Recovery(BlobLayer::Recovery& blobLayer)
    : blobLayerRecovery(blobLayer) {
  for (auto& staging: blobLayer.recoverTemporaries(RecoveryType::STAGING)) {
    uint64_t id = staging->getOldId().id;
    recoveredStaging.insert(std::make_pair(id, kj::mv(staging)));
  }

  for (auto type: ALL_RECOVERY_TYPES) {
    if (type != RecoveryType::STAGING && type != RecoveryType::JOURNAL) {
      for (auto& temp: blobLayer.recoverTemporaries(type)) {
        RecoveryId id = temp->getOldId();
        JournalLayer& super = *this;
        recoveredTemporaries.insert(std::make_pair(id,
            kj::heap<RecoveredTemporary>(super, kj::mv(temp))));
      }
    }
  }

  auto journals = blobLayer.recoverTemporaries(RecoveryType::JOURNAL);
  KJ_ASSERT(journals.size() <= 1);
  if (journals.size() == 1) {
    commitSavedTransaction(journals[0]->getContent());
  }
}

kj::Maybe<kj::Own<JournalLayer::Object>> JournalLayer::Recovery::getObject(ObjectId id) {
  KJ_REQUIRE(!finished, "already called finish()");

  auto iter = openObjects.find(id);
  if (iter != openObjects.end()) {
    return kj::addRef(*iter->second);
  }

  return blobLayerRecovery.getObject(id).map([this,id](kj::Own<BlobLayer::Object>&& object) {
    JournalLayer& super = *this;
    return kj::refcounted<JournalLayer::Object>(super, id, kj::mv(object));
  });
}

kj::Array<kj::Own<JournalLayer::RecoveredTemporary>> JournalLayer::Recovery::recoverTemporaries(
    RecoveryType type) {
  KJ_REQUIRE(!finished, "already called finish()");

  kj::Vector<kj::Own<JournalLayer::RecoveredTemporary>> results;

  auto begin = recoveredTemporaries.lower_bound(RecoveryId(type, 0));
  auto end = recoveredTemporaries.lower_bound(RecoveryId(
      static_cast<RecoveryType>(static_cast<uint>(type) + 1), 0));

  for (auto i = begin; i != end; ++i) {
    results.add(kj::mv(i->second));
  }

  recoveredTemporaries.erase(begin, end);

  return results.releaseAsArray();
}

void JournalLayer::Recovery::commitSavedTransaction(BlobLayer::Content& content) {
  KJ_REQUIRE(!finished, "already called finish()");

  uint64_t start = content.getStart();
  uint64_t end = content.getSize().endMarker;

  auto entries = kj::heapArray<JournalEntry>((end - start) / sizeof(JournalEntry));

  content.read(start, entries.asBytes());

  uint32_t expectedTxSize = 0;
  JournalEntry* txnStart = entries.begin();
  for (auto& entry: entries) {
    if (expectedTxSize > 0 && entry.txSize != expectedTxSize) {
      // It would seem that the journal is invalid starting here, perhaps because the last
      // transaction had only been partially flushed to disk. In particular it's possible for
      // the file end pointer to be updated before the actual content has been flushed, leaving
      // trailing garbage (usually zeros).
      break;
    }

    expectedTxSize = entry.txSize - 1;

    if (expectedTxSize == 0) {
      // This is the last entry in a transaction!
      for (auto& entryToReplay: kj::arrayPtr(txnStart, &entry + 1)) {
        replayEntry(blobLayerRecovery, entryToReplay);
      }
      txnStart = &entry + 1;
    }
  }
}

JournalLayer& JournalLayer::Recovery::finish() {
  KJ_REQUIRE(!finished, "already called finish()");

  finished = true;
  recoveredStaging.clear();
  recoveredTemporaries.clear();

  // Init JournalLayer members.
  blobLayer = &blobLayerRecovery.finish();
  journalFile = blobLayer->newTemporary();
  writeQueue = kj::READY_NOW;

  return *this;
}

void JournalLayer::Recovery::replayEntry(
    BlobLayer::Recovery& blobLayer, const JournalEntry& entry) {
  kj::Own<BlobLayer::RecoveredTemporary> staging;

  switch (entry.type) {
    case JournalEntry::Type::CREATE_OBJECT:
    case JournalEntry::Type::UPDATE_OBJECT:
    case JournalEntry::Type::CREATE_TEMPORARY:
    case JournalEntry::Type::UPDATE_TEMPORARY: {
      auto iter = recoveredStaging.find(entry.stagingId);
      if (iter == recoveredStaging.end()) {
        // This operation must have already been carried out, as the source staging file is
        // absent.
        return;
      }
      staging = kj::mv(iter->second);
      recoveredStaging.erase(iter);
      break;
    }

    case JournalEntry::Type::UPDATE_XATTR:
    case JournalEntry::Type::UPDATE_TEMPORARY_XATTR:
    case JournalEntry::Type::DELETE_OBJECT:
    case JournalEntry::Type::DELETE_TEMPORARY:
      break;
  }

  switch (entry.type) {
    case JournalEntry::Type::CREATE_OBJECT:
      staging->keepAs(entry.object.id, entry.object.xattr);
      return;

    case JournalEntry::Type::UPDATE_OBJECT:
      staging->keepAs(entry.object.id, entry.object.xattr);
      return;

    case JournalEntry::Type::UPDATE_XATTR:
      KJ_IF_MAYBE(object, blobLayer.getObject(entry.object.id)) {
        object->get()->setXattr(entry.object.xattr);
      }
      return;

    case JournalEntry::Type::DELETE_OBJECT:
      KJ_IF_MAYBE(object, blobLayer.getObject(entry.object.id)) {
        object->get()->remove();
      }
      return;

    case JournalEntry::Type::CREATE_TEMPORARY: {
      if (recoveredTemporaries.count(entry.temporary.id) == 0) {
        JournalLayer& super = *this;
        recoveredTemporaries[entry.temporary.id] =
            kj::heap<JournalLayer::RecoveredTemporary>(super,
                entry.temporary.id, entry.temporary.xattr, kj::mv(staging));
      }
      return;
    }

    case JournalEntry::Type::UPDATE_TEMPORARY: {
      auto iter = recoveredTemporaries.find(entry.temporary.id);
      if (iter != recoveredTemporaries.end()) {
        iter->second->overwrite(entry.temporary.xattr, kj::mv(staging));
      }
      return;
    }

    case JournalEntry::Type::UPDATE_TEMPORARY_XATTR: {
      auto iter = recoveredTemporaries.find(entry.temporary.id);
      if (iter != recoveredTemporaries.end()) {
        iter->second->setXattr(entry.temporary.xattr);
      }
      return;
    }

    case JournalEntry::Type::DELETE_TEMPORARY:
      recoveredTemporaries.erase(entry.temporary.id);
      return;
  }

  KJ_UNREACHABLE;
}

}  // namespace storage
}  // namespace blackrock
