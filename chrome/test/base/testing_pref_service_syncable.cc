// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/base/testing_pref_service_syncable.h"

#include "base/bind.h"
#include "base/prefs/pref_notifier_impl.h"
#include "base/prefs/pref_value_store.h"
#include "components/user_prefs/pref_registry_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

template<>
TestingPrefServiceBase<PrefServiceSyncable, PrefRegistrySyncable>::
TestingPrefServiceBase(TestingPrefStore* managed_prefs,
                       TestingPrefStore* user_prefs,
                       TestingPrefStore* recommended_prefs,
                       PrefRegistrySyncable* pref_registry,
                       PrefNotifierImpl* pref_notifier)
    : PrefServiceSyncable(
        pref_notifier,
        new PrefValueStore(
            managed_prefs,
            NULL,
            NULL,
            user_prefs,
            recommended_prefs,
            pref_registry->defaults(),
            pref_notifier),
        user_prefs,
        pref_registry,
        base::Bind(
            &TestingPrefServiceBase<PrefServiceSyncable,
                                    PrefRegistrySyncable>::HandleReadError),
        false),
      managed_prefs_(managed_prefs),
      user_prefs_(user_prefs),
      recommended_prefs_(recommended_prefs) {
}

TestingPrefServiceSyncable::TestingPrefServiceSyncable()
    : TestingPrefServiceBase<PrefServiceSyncable, PrefRegistrySyncable>(
        new TestingPrefStore(),
        new TestingPrefStore(),
        new TestingPrefStore(),
        new PrefRegistrySyncable(),
        new PrefNotifierImpl()) {
}

TestingPrefServiceSyncable::~TestingPrefServiceSyncable() {
}

PrefRegistrySyncable* TestingPrefServiceSyncable::registry() {
  return static_cast<PrefRegistrySyncable*>(DeprecatedGetPrefRegistry());
}
