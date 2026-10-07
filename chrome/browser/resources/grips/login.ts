// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const connectURL = 'https://connect.cs.ac.cn/';
const button = document.querySelector<HTMLButtonElement>('#login');
const status = document.querySelector<HTMLElement>('#status');

function openConnect() {
  // A top-level navigation is intentional: Connect's host-only cookies are
  // stored in the normal Chromium profile and are then reused by the exam's
  // standard OIDC authorization-code flow. An iframe would make this a
  // third-party cookie and would be blocked by modern cookie policies.
  window.location.replace(connectURL);
}

button?.addEventListener('click', openConnect);

// Match the browser's normal sign-in affordance: opening grips://login starts
// the Connect flow immediately, while leaving a button for blocked scripts or
// users who want to retry after a transient network failure. The `manual`
// query is used by browser tests to keep the WebUI document committed.
if (!new URLSearchParams(window.location.search).has('manual')) {
  window.setTimeout(() => {
    if (document.visibilityState === 'visible') {
      openConnect();
    }
  }, 100);
}

if (status) {
  status.textContent = 'Connect login opens in this tab; after signing in, open an exam link.';
}
