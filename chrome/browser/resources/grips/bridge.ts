(() => {
  'use strict';

  const examOrigin = 'https://exam.cs.ac.cn';
  const allowedMessages = new Set([
    'setByodTunnelConfig',
    'clearByodTunnelConfig',
    'setByodFullscreen',
  ]);
  const frame = document.getElementById('exam-app');
  const loading = document.getElementById('bridge-loading');
  if (!(frame instanceof HTMLIFrameElement)) {
    return;
  }

  const topURL = new URL(window.location.href);
  const remoteURL = new URL(examOrigin);
  remoteURL.pathname = topURL.pathname || '/';
  remoteURL.search = topURL.search;
  remoteURL.hash = topURL.hash;
  frame.src = remoteURL.href;

  window.addEventListener('message', (event) => {
    if (event.source !== frame.contentWindow || event.origin !== examOrigin) {
      return;
    }
    const message = event.data;
    if (message?.channel === 'byod-browser' && message.type === 'ready') {
      frame.contentWindow?.postMessage(
          {channel: 'byod-browser', type: 'ready'}, examOrigin);
      return;
    }
    if (!message || message.channel !== 'byod-browser' ||
        !allowedMessages.has(message.name)) {
      return;
    }
    const chromeApi = (window as Window & {
      chrome?: {send?: (name: string, args: unknown[]) => void},
    }).chrome;
    if (chromeApi && typeof chromeApi.send === 'function') {
      chromeApi.send(message.name, message.args || []);
    }
  });

  frame.addEventListener('load', () => {
    if (loading instanceof HTMLElement) {
      loading.hidden = true;
    }
    frame.contentWindow?.postMessage(
        {channel: 'byod-browser', type: 'ready'}, examOrigin);
  });
})();
