// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('options', function() {
  /** @const */ var OptionsPage = options.OptionsPage;

  //////////////////////////////////////////////////////////////////////////////
  // ManagedUserSetPassphraseOverlay class:

  /**
   * Encapsulated handling of the Managed User Set Passphrase page.
   * @constructor
   */
  function ManagedUserSetPassphraseOverlay() {
    OptionsPage.call(
      this,
      'setPassphrase',
      loadTimeData.getString('setPassphraseTitle'),
      'managed-user-set-passphrase-overlay');
  }

  cr.addSingletonGetter(ManagedUserSetPassphraseOverlay);

  /** Closes the page and resets the passphrase fields */
  var closePage = function(container) {
    // Reseting the fields directly would lead to a flicker, so listen to
    // webkitTransitionEnd to clear them after that. Similar to how it is done
    // in setOverlayVisible_ in options_page.js.
    container.addEventListener('webkitTransitionEnd', function f(e) {
      if (e.target != e.currentTarget || e.propertyName != 'opacity')
        return;
      container.removeEventListener('webkitTransitionEnd', f);

      // Reset the fields.
      $('managed-user-passphrase').value = '';
      $('passphrase-confirm').value = '';
      $('passphrase-mismatch').hidden = true;
    });
    OptionsPage.closeOverlay();
  };

  ManagedUserSetPassphraseOverlay.prototype = {
    __proto__: OptionsPage.prototype,

    /** @override */
    initializePage: function() {
      OptionsPage.prototype.initializePage.call(this);
      $('managed-user-passphrase').oninput = this.updateDisplay_;
      $('passphrase-confirm').oninput = this.updateDisplay_;

      $('save-passphrase').onclick = this.setPassphrase_.bind(this);

      $('managed-user-passphrase').onkeypress = function(event) {
        // Check if the user pressed enter and advance to the
        // confirmation field since that was probably the intention.
        if (event.keyCode == 13)
          $('passphrase-confirm').focus();
      };

      var self = this;
      $('passphrase-confirm').onkeypress = function(event) {
        // Allow submitting only valid information.
        if (!$('passphrase-confirm').validity.valid ||
            !$('managed-user-passphrase').validity.valid) {
          return;
        }
        // Check if the user pressed enter.
        if (event.keyCode == 13)
          self.setPassphrase_(event);
      };

      $('cancel-passphrase').onclick = function(event) {
        closePage(self.container);
      };
    },

    /** Updates the display according to the validity of the user input. */
    updateDisplay_: function() {
      var valid =
          $('passphrase-confirm').value == $('managed-user-passphrase').value;
      $('passphrase-mismatch').hidden = valid;
      $('passphrase-confirm').setCustomValidity(
          valid ? '' : $('passphrase-mismatch').textContent);
      $('save-passphrase').disabled =
          !$('passphrase-confirm').validity.valid ||
          !$('managed-user-passphrase').validity.valid;
    },
    /**
     * Sets the passphrase and closes the overlay.
     * @param {Event} event The event that triggered the call to this function.
     */
    setPassphrase_: function(event) {
      chrome.send('setPassphrase', [$('managed-user-passphrase').value]);
      closePage(this.container);
    },

    /** @override */
    canShowPage: function() {
      return ManagedUserSettings.getInstance().authenticationState ==
          options.ManagedUserAuthentication.AUTHENTICATED;
    },

    /** @override */
    didShowPage: function() {
      $('managed-user-passphrase').focus();
    },

    /**
     * Make sure that we reset the fields on cancel.
     */
    handleCancel: function() {
      closePage(this.container);
    }
  };

  /** Used for browsertests. */
  var ManagedUserSetPassphraseForTesting = {
    getPassphraseInput: function() {
      return $('managed-user-passphrase');
    },
    getPassphraseConfirmInput: function() {
      return $('passphrase-confirm');
    },
    getSavePassphraseButton: function() {
      return $('save-passphrase');
    },
    updateDisplay: function() {
      return ManagedUserSetPassphraseOverlay.getInstance().updateDisplay_();
    }
  };

  // Export
  return {
    ManagedUserSetPassphraseOverlay: ManagedUserSetPassphraseOverlay,
    ManagedUserSetPassphraseForTesting: ManagedUserSetPassphraseForTesting
  };

});
