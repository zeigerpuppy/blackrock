// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2015 Sandstorm Development Group, Inc. and contributors
// All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

var Crypto = Npm.require("crypto");
var HOSTNAME = process.env.ROOT_URL;
var stripe = Npm.require("stripe")(Meteor.settings.stripeKey);

BlackrockPayments = {};

var serveCheckout = Meteor.bindEnvironment(function (res) {
  res.writeHead(200, { "Content-Type": "text/html" });
  res.end(Assets.getText("checkout.html").replace(
      "$STRIPE_KEY", Meteor.settings.public.stripePublicKey));
});

var serveSandcat = Meteor.bindEnvironment(function (res) {
  res.writeHead(200, { "Content-Type": "image/png" });
  // Meteor's buffer isn't a real buffer, so we have to do a copy
  res.end(new Buffer(Assets.getBinary("sandstorm-purplecircle.png")));
});

function hashId(id) {
  return Crypto.createHash("sha256").update(HOSTNAME + ":" + id).digest("base64");
}

function findOriginalId(hashedId, customerId) {
  var data = Meteor.wrapAsync(stripe.customers.retrieve.bind(stripe.customers))(customerId);
  if (data.sources && data.sources.data) {
    var sources = data.sources.data;
    for (var i = 0; i < sources.length; i++) {
      if (hashId(sources[i].id) === hashedId) {
        return sources[i].id;
      }
    }
  }

  throw new Meteor.Error(400, "Id not found");
}

function sanitizeSource(source, isPrimary) {
  var result = _.pick(source, "last4", "brand", "exp_year", "exp_month", "isPrimary");
  result.isPrimary = isPrimary;
  result.id = hashId(source.id);
  return result;
}

BlackrockPayments.makeConnectHandler = function (db) {
  return function (req, res, next) {
    if (req.headers.host == db.makeWildcardHost("payments")) {
      if (req.url == "/checkout") {
        serveCheckout(res);
      } else if (req.url == "/sandstorm-purplecircle.png") {
        serveSandcat(res);
      } else {
        res.writeHead(404, { "Content-Type": "text/plain" });
        res.end("404 not found: " + req.url);
      }
    } else {
      next();
    }
  };
}

function createUser(token, email) {
  var data = Meteor.wrapAsync(stripe.customers.create.bind(stripe.customers))({
    source: token,
    email: email,
    description: Meteor.userId()  // TODO(soon): Do we want to store backrefs to our database in stripe?
  });
  Meteor.users.update({_id: Meteor.userId()}, {$set: {payments: {id: data.id}}});
  return data;
}

var methods = {
  addCardForUser: function (token, email) {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to add card");
    }
    check(token, String);
    check(email, String);

    var user = Meteor.user();

    if (user.payments && user.payments.id) {
      return sanitizeSource(Meteor.wrapAsync(stripe.customers.createSource.bind(stripe.customers))(
        user.payments.id,
        {source: token}
      ), false);
    } else {
      var data = createUser(token, email);
      if (data.sources && data.sources.data && data.sources.data.length >= 1) {
        return sanitizeSource(data.sources.data[0], true);
      }
    }
  },

  deleteCardForUser: function (id) {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to delete card");
    }
    check(id, String);

    var customerId = Meteor.user().payments.id;
    var data = Meteor.wrapAsync(stripe.customers.retrieve.bind(stripe.customers))(customerId);
    if (data.sources && data.sources.data && data.subscriptions && data.subscriptions.data) {
      var sources = data.sources.data;
      var subscriptions = data.subscriptions.data;
      if (sources.length == 1 && subscriptions.length > 0) {
        // TODO(soon): handle this better (client-side?)
        throw new Meteor.Error(400, "Can't delete last card if still subscribed");
      }
    }

    id = findOriginalId(id, customerId);

    Meteor.wrapAsync(stripe.customers.deleteCard.bind(stripe.customers))(
      customerId,
      id
    );
  },

  makeCardPrimary: function (id) {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to change primary card");
    }
    check(id, String);

    var customerId = Meteor.user().payments.id;
    id = findOriginalId(id, customerId);

    Meteor.wrapAsync(stripe.customers.update.bind(stripe.customers))(
      customerId,
      {default_source: id}
    );
  },

  getStripeData: function () {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to get stripe data");
    }
    var payments = Meteor.user().payments;
    if (!payments) {
      return {};
    }
    var customerId = payments.id;
    var data = Meteor.wrapAsync(stripe.customers.retrieve.bind(stripe.customers))(customerId);
    if (data.sources && data.sources.data) {
      var sources = data.sources.data;
      for (var i = 0; i < sources.length; i++) {
        sources[i] = sanitizeSource(sources[i], sources[i].id === data.default_source);
      }
    }

    var subscription;
    if (data.subscriptions && data.subscriptions.data[0]) {
      // Plan names end with "-beta".
      subscription = data.subscriptions.data[0].plan.id.split("-")[0];
    }
    return {
      email: data.email,
      subscription: subscription,
      sources: data.sources && data.sources.data,
      credit: -(data.account_balance || -0)
    };
  },

  updateUserSubscription: function (newPlan) {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to update subscription");
    }
    check(newPlan, String);

    var planInfo = this.connection.sandstormDb.getPlan(newPlan);

    var payments = Meteor.user().payments;
    if (!payments) {
      throw new Meteor.Error(403, "User must have stripe data already");
    }
    var customerId = payments.id;
    var data = Meteor.wrapAsync(stripe.customers.retrieve.bind(stripe.customers))(customerId);

    if (newPlan === "free") {
      if (data.subscriptions && data.subscriptions.data.length > 0) {
        // TODO(someday): pass in at_period_end and properly handle pending cancelled subscriptions
        Meteor.wrapAsync(stripe.customers.cancelSubscription.bind(stripe.customers))(
          customerId,
          data.subscriptions.data[0].id
        );
      }
      // else: no subscriptions exist so we're already set to free
    } else {
      if (data.subscriptions && data.subscriptions.data.length > 0) {
        Meteor.wrapAsync(stripe.customers.updateSubscription.bind(stripe.customers))(
          customerId,
          data.subscriptions.data[0].id,
          {plan: newPlan + "-beta"}
        );
      } else {
        Meteor.wrapAsync(stripe.customers.createSubscription.bind(stripe.customers))(
          customerId,
          {plan: newPlan + "-beta"}
        );
      }
    }

    Meteor.users.update({_id: this.userId}, {$set: { plan: newPlan }});
  },

  createUserSubscription: function (token, email, plan) {
    if (!this.userId) {
      throw new Meteor.Error(403, "Must be logged in to update subscription");
    }
    check(token, String);
    check(email, String);
    check(plan, String);

    var payments = Meteor.user().payments;
    var customerId;
    var sanitizedSource;
    if (!payments || !payments.id) {
      var data = createUser(token, email);
      customerId = data.id;
      if (data.sources && data.sources.data && data.sources.data.length >= 1) {
        sanitizedSource = sanitizeSource(data.sources.data[0]);
      }
    } else {
      customerId = payments.id;
      sanitizedSource = methods.addCardForUser.bind(this)(token, email);
    }
    Meteor.wrapAsync(stripe.customers.createSubscription.bind(stripe.customers))(
      customerId,
      {plan: plan + "-beta"}
    );
    Meteor.users.update({_id: this.userId}, {$set: { plan: plan }});
    return sanitizedSource;
  }
};
Meteor.methods(methods);

Meteor.publish("plans", function () {
  return this.connection.sandstormDb.listPlans();
});

function getAllStripeCustomers() {
  var hasMore = true;
  var results = [];

  while (hasMore) {
    var next = Meteor.wrapAsync(stripe.customers.list.bind(stripe.customers))({limit: 100});
    results = results.concat(next.data);
    hasMore = next.has_more;
  }
  return results;
}

SandstormDb.paymentsMigrationHook = function (SignupKeys, plans) {
  var db = this;
  var customers = getAllStripeCustomers();
  if (!customers) throw new Error("missing customers");

  var byEmail = {};
  for (var i in customers) {
    var customer = customers[i];
    byEmail[customer.email] = byEmail[customer.email] || [];
    byEmail[customer.email].push(customer);
  }

  var byQuota = {};
  for (var i in plans) {
    if (plans[i]._id !== "free") {
      byQuota[plans[i].storage] = plans[i]._id;
    }
  }

  function getCustomerByEmail(email, quota) {
    var customer = (byEmail[email] || []).shift();
    if (customer) {
      var plan;
      if (customer.subscriptions && customer.subscriptions.data[0]) {
        // Plan names end with "-beta".
        plan = customer.subscriptions.data[0].plan.id.split("-")[0];
      } else {
        plan = "free";
      }
      return {plan: plan, payments: {id: customer.id}};
    }
  }

  Meteor.users.find({quota: {$exists: true}}).forEach(function (user) {
    if (user.signupEmail) {
      var customer = getCustomerByEmail(user.signupEmail, user.quota);
      if (customer) {
        console.log("user", user._id, user.signupEmail, "=>", JSON.stringify(customer));
        Meteor.users.update({_id: user._id}, {$set: customer});
      } else {
        console.error("ERROR: missing customer for email (user):", email, user._id);
      }
    } else {
      console.warn("WARNING: user was not invited by email:",
          user._id, SandstormDb.getUserIdentities(user));
    }
  });

  SignupKeys.find({quota: {$exists: true}, used: false}).forEach(function (signupKey) {
    if (signupKey.email) {
      var customer = getCustomerByEmail(signupKey.email, signupKey.quota);
      if (customer) {
        console.log("invite", signupKey.email, "=>", JSON.stringify(customer));
        SignupKeys.update({_id: signupKey._id}, {$set: customer});
      } else {
        console.error("ERROR: missing customer for email (invite):", email, signupKey._id);
      }
    } else {
      console.warn("WARNING: non-email invite:", signupKey.note);
    }
  });

  for (var email in byEmail) {
    byEmail[email].forEach(function (customer) {
      console.error("ERROR: customer not used:", customer.id, email);
    });
  }
}