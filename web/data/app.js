// Intercom Web Client
function createWebPeerIdentity() {
  const randomBytes = new Uint8Array(6);
  if (window.crypto && window.crypto.getRandomValues) {
    window.crypto.getRandomValues(randomBytes);
  } else {
    for (let i = 0; i < randomBytes.length; i++) {
      randomBytes[i] = Math.floor(Math.random() * 256);
    }
  }
  randomBytes[0] = (randomBytes[0] & 0xFE) | 0x02;
  return Array.from(randomBytes, byte =>
    byte.toString(16).padStart(2, '0')).join(':').toUpperCase();
}

const webPeerIdentity = createWebPeerIdentity();
const webPeerName = 'Web-' + webPeerIdentity.replace(/:/g, '').slice(-10);

const CONFIG = {
  webPeerName,
  webPeerIdentity,
  wsUrl: (window.location.protocol === 'https:' ? 'wss://' : 'ws://') +
    window.location.host + '/ws',
  reconnectDelay: 2000,
  maxReconnectDelay: 30000,
  connectTimeout: 10000,
  peerAnnouncementInterval: 2000,
  audioSampleRate: 16000,
  frameMs: 20,
  frameSamples: 320,
  frameBytes: 160
};

// Spy commands (matching firmware)
const SPY_CMD = {
  LIVE_START: 1,
  LIVE_STOP: 2,
  CLIP_START: 3,
  CLIP_STOP: 4,
  CLIP_DONE: 5,
  BUSY: 6
};

let ws = null;
let reconnectAttempts = 0;
let reconnectTimer = null;
let connectionTimer = null;
let helloTimer = null;
let audioContext = null;
let micStream = null;
let micSource = null;
let micGainNode = null;
let processor = null;
let audioCaptureStarting = false;
let micStatusText = '';
let micStatusClass = '';
let captureFrame = new Float32Array(CONFIG.frameSamples);
let captureFrameOffset = 0;
let resampleAccumulator = 0;
let audioFramesQueued = 0;
let lastTxStatusUpdate = 0;
let sendErrorLogged = false;
let isTalking = false;
let isMuted = false;
let isRecording = false;
let recordedFrames = [];
let playbackQueue = [];
let playbackNextTime = 0;

let adpcmPredictor = 0;
let adpcmIndex = 0;
let txSequence = 0;
let remoteSpyLive = false;
let remoteSpyLiveTimer = null;
let remoteSpyRecording = false;
let remoteSpyFrames = [];
let remoteSpyClipTimer = null;

// Spy state
let currentMode = 'live';
let selectedPeer = null;
let peers = [];
let isSpyLive = false;
let isSpyClip = false;
let spyTargetPeer = null;

const statusEl = document.getElementById('status');
const peerNameEl = document.querySelector('.peer-name');
const peerRssiEl = document.querySelector('.peer-rssi');
const micLevelEl = document.querySelector('.mic-level');
const micLevelValueEl = document.getElementById('mic-level-value');
const micStatusEl = document.getElementById('mic-status');
const rxLevelEl = document.querySelector('.rx-level');
const txStatusEl = document.getElementById('tx-status');
const talkBtn = document.getElementById('talk-btn');
const clipBtn = document.getElementById('clip-btn');
const muteBtn = document.getElementById('mute-btn');
const volumeSlider = document.getElementById('volume');
const micGainValueEl = document.getElementById('mic-gain-value');
const modeToggleEl = document.getElementById('mode-toggle');
const logEl = document.getElementById('log');
const peerListEl = document.getElementById('peer-list');
const peerItemsEl = document.getElementById('peer-items');
const spyControlsEl = document.getElementById('spy-controls');
const spyLiveBtn = document.getElementById('spy-live-btn');
const spyClipBtn = document.getElementById('spy-clip-btn');
const spyStopBtn = document.getElementById('spy-stop-btn');

function log(msg) {
  const time = new Date().toLocaleTimeString();
  logEl.innerHTML = time + ' ' + msg + '<br>' + logEl.innerHTML;
  if (logEl.children.length > 20) {
    logEl.removeChild(logEl.lastChild);
  }
}

function connectWebSocket() {
  if (ws && ws.readyState !== WebSocket.CLOSED) return;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  updateStatus('connecting');
  log('Connecting to intercom...');
  try {
    const socket = new WebSocket(CONFIG.wsUrl);
    ws = socket;
    socket.binaryType = 'arraybuffer';
    socket.onopen = () => {
      if (ws !== socket) return;
      clearTimeout(connectionTimer);
      connectionTimer = null;
      reconnectAttempts = 0;
      audioFramesQueued = 0;
      updateStatus('connected');
      updateTxStatus(true);
      setMicStatus('Requesting microphone permission');
      log('Connected to intercom');
      sendHello();
      clearInterval(helloTimer);
      helloTimer = setInterval(() => {
        if (ws === socket && socket.readyState === WebSocket.OPEN) sendHello();
      }, CONFIG.peerAnnouncementInterval);
      sendAudioTarget();
      startAudioCapture();
    };
    socket.onclose = (event) => {
      if (ws !== socket) return;
      clearTimeout(connectionTimer);
      connectionTimer = null;
      clearInterval(helloTimer);
      helloTimer = null;
      updateStatus('disconnected');
      log('Disconnected (code ' + event.code + '): ' +
          (event.reason || 'connection lost'));
      ws = null;
      stopTalking();
      peers = [];
      selectedPeer = null;
      peerNameEl.textContent = 'All peers';
      peerRssiEl.textContent = 'Broadcasting';
      remoteSpyLive = false;
      clearTimeout(remoteSpyLiveTimer);
      remoteSpyLiveTimer = null;
      remoteSpyRecording = false;
      remoteSpyFrames = [];
      clearTimeout(remoteSpyClipTimer);
      remoteSpyClipTimer = null;
      renderPeerList();
      updateUIForMode();
      updateTxStatus(true);
      scheduleReconnect();
    };
    socket.onerror = () => {
      if (ws !== socket) return;
      log('WebSocket error');
    };
    socket.onmessage = (event) => {
      if (ws !== socket) return;
      if (event.data instanceof ArrayBuffer) handleBinaryPacket(event.data);
      else handleTextMessage(event.data);
    };
    connectionTimer = setTimeout(() => {
      if (ws === socket && socket.readyState === WebSocket.CONNECTING) {
        log('WebSocket connection timed out');
        socket.close();
      }
    }, CONFIG.connectTimeout);
  } catch (e) {
    ws = null;
    log('Connection failed: ' + e.message);
    scheduleReconnect();
  }
}

function scheduleReconnect() {
  if (reconnectTimer || (ws && ws.readyState !== WebSocket.CLOSED)) return;
  const delay = Math.min(CONFIG.reconnectDelay * Math.pow(2, reconnectAttempts), CONFIG.maxReconnectDelay);
  reconnectAttempts++;
  log('Reconnecting in ' + (delay / 1000).toFixed(1) + 's...');
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, delay);
}

function updateStatus(state) {
  statusEl.className = 'status ' + state;
  statusEl.textContent = state.charAt(0).toUpperCase() + state.slice(1);
}

function samePeer(peer, candidate) {
  if (Boolean(peer.webPeer) !== Boolean(candidate.webPeer)) return false;
  if (peer.webPeer) {
    if (peer.identityMac && candidate.identityMac) {
      return peer.identityMac.toUpperCase() === candidate.identityMac.toUpperCase();
    }
    return peer.id === candidate.id ||
      (peer.name && candidate.name && peer.name === candidate.name);
  }
  return peer.id === candidate.id ||
    (peer.name && candidate.name && peer.name === candidate.name) ||
    (peer.mac && candidate.mac &&
      peer.mac.toUpperCase() === candidate.mac.toUpperCase());
}

function normalizePeers(list) {
  const normalized = [];
  list.forEach(peer => {
    const candidate = {
      ...peer,
      id: peer.id || (peer.mac + '/' + (peer.port || 0))
    };
    const duplicateIndex = normalized.findIndex(existing =>
      samePeer(existing, candidate));
    if (duplicateIndex < 0) {
      normalized.push(candidate);
      return;
    }

    const existing = normalized[duplicateIndex];
    normalized[duplicateIndex] = {
      ...existing,
      ...candidate,
      id: candidate.id,
      mac: candidate.mac || existing.mac,
      port: candidate.port === undefined ? existing.port : candidate.port,
      viaWifi: candidate.viaWifi === undefined
        ? existing.viaWifi : candidate.viaWifi,
      webPeer: Boolean(candidate.webPeer)
    };
  });
  return normalized;
}

function sendHello() {
  sendPacket(JSON.stringify({
    cmd: 'hello',
    name: CONFIG.webPeerName,
    identityMac: CONFIG.webPeerIdentity
  }));
}

function sendAudioTarget() {
  const peer = selectedPeer;
  sendPacket(JSON.stringify({
    cmd: 'target',
    id: peer ? peer.id : 'all',
    addr: peer ? peer.mac || '' : '',
    identityMac: peer ? peer.identityMac || '' : '',
    port: peer ? Number(peer.port || 0) : 0,
    viaWifi: peer ? Boolean(peer.viaWifi) : false,
    webPeer: peer ? Boolean(peer.webPeer) : false
  }));
}

function setMicStatus(message, state) {
  if (!micStatusEl || (micStatusText === message && micStatusClass === state)) return;
  micStatusText = message;
  micStatusClass = state || '';
  micStatusEl.textContent = message;
  micStatusEl.className = 'mic-status' + (micStatusClass ? ' ' + micStatusClass : '');
}

function sendPacket(data) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    updateTxStatus(true);
    return false;
  }
  try {
    ws.send(data);
    sendErrorLogged = false;
    if (data instanceof ArrayBuffer && data.byteLength >= 3 &&
        new DataView(data).getUint8(2) === 2) {
      audioFramesQueued++;
      updateTxStatus();
    }
    return true;
  } catch (e) {
    if (!sendErrorLogged) {
      log('WebSocket send failed: ' + e.message);
      sendErrorLogged = true;
    }
    updateTxStatus(true);
    return false;
  }
}

function updateTxStatus(force) {
  if (!txStatusEl) return;
  const now = Date.now();
  if (!force && now - lastTxStatusUpdate < 250) return;
  lastTxStatusUpdate = now;
  const connected = ws && ws.readyState === WebSocket.OPEN;
  const transmitting = isTalking || isRecording || remoteSpyLive ||
    remoteSpyRecording;
  txStatusEl.textContent = connected
    ? (transmitting ? 'Sending' : 'Idle') + ' · ' + audioFramesQueued +
      ' frames queued · ' + ws.bufferedAmount + ' B buffered'
    : 'Offline';
  txStatusEl.classList.toggle('transmitting', Boolean(connected && transmitting));
}

function sendSpyCommand(cmd, peer) {
  if (!peer || !peer.mac) return;
  sendPacket(JSON.stringify({
    cmd: 'spy',
    action: cmd,
    addr: peer.mac,
    port: Number(peer.port || 0),
    identityMac: peer.identityMac || '',
    viaWifi: Boolean(peer.viaWifi),
    webPeer: Boolean(peer.webPeer)
  }));
}

function createSpyPacket(cmd) {
  const headerSize = 10;
  const payloadSize = 1;
  const packet = new ArrayBuffer(headerSize + payloadSize);
  const view = new DataView(packet);
  const bytes = new Uint8Array(packet);

  view.setUint8(0, 0x49); // magic 'I'
  view.setUint8(1, 1);    // version
  view.setUint8(2, 3);    // type = spy
  view.setUint8(3, 0);    // flags
  view.setUint16(4, 0, true); // seq
  view.setInt16(6, 0, true);  // predictor (unused for spy)
  view.setUint8(8, 0);    // adpcmIndex (unused for spy)
  view.setUint8(9, payloadSize);

  bytes[headerSize] = cmd;
  return packet;
}

async function startAudioCapture(userInitiated) {
  if (audioCaptureStarting) {
    if (userInitiated && audioContext && audioContext.state === 'suspended') {
      try {
        await audioContext.resume();
      } catch (e) {
        setMicStatus('Tap again to enable microphone audio', 'error');
        log('Audio context resume failed: ' + e.message);
      }
    }
    return;
  }
  const hasLiveMic = micStream && micStream.getAudioTracks().some(
    track => track.readyState === 'live');
  if (hasLiveMic && audioContext && audioContext.state !== 'closed' &&
      processor) {
    if (audioContext.state === 'suspended') {
      try {
        await audioContext.resume();
      } catch (e) {
        setMicStatus('Tap Hold to Talk to enable microphone audio', 'error');
        log('Audio context resume failed: ' + e.message);
      }
    }
    if (audioContext.state === 'running') {
      setMicStatus('Microphone active; hold to send', 'active');
    }
    return;
  }

  audioCaptureStarting = true;
  try {
    if (processor) {
      processor.onaudioprocess = null;
      processor.disconnect();
      processor = null;
    }
    if (micSource) {
      micSource.disconnect();
      micSource = null;
    }
    if (micGainNode) {
      micGainNode.disconnect();
      micGainNode = null;
    }
    if (micStream) {
      micStream.getTracks().forEach(track => track.stop());
    }
    if (audioContext && audioContext.state !== 'closed') {
      await audioContext.close();
    }

    audioContext = new (window.AudioContext || window.webkitAudioContext)();
    if (userInitiated && audioContext.state === 'suspended') {
      await audioContext.resume();
    }
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        sampleRate: { ideal: CONFIG.audioSampleRate },
        channelCount: { ideal: 1 },
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true
      }
    });
    micStream = stream;
    resampleAccumulator = 0;
    playbackNextTime = 0;
    stream.getAudioTracks().forEach(track => {
      track.onended = () => {
        if (micStream === stream) {
          setMicStatus('Microphone disconnected', 'error');
          micLevelEl.style.width = '0%';
          micLevelValueEl.textContent = '0%';
        }
      };
      track.onmute = () => {
        if (micStream === stream) setMicStatus('Microphone is muted', 'error');
      };
      track.onunmute = () => {
        if (micStream === stream && audioContext.state === 'running') {
          setMicStatus('Microphone active; hold to send', 'active');
        }
      };
    });
    micSource = audioContext.createMediaStreamSource(micStream);
    micGainNode = audioContext.createGain();
    micGainNode.gain.value = Number(volumeSlider.value) / 100;
    processor = audioContext.createScriptProcessor(512, 1, 1);
    processor.onaudioprocess = (e) => {
      collectAudioFrames(e.inputBuffer.getChannelData(0));
    };
    micSource.connect(micGainNode);
    micGainNode.connect(processor);
    processor.connect(audioContext.destination);
    if (audioContext.state === 'suspended') {
      try {
        await audioContext.resume();
      } catch (e) {
        setMicStatus('Tap Hold to Talk to enable microphone audio', 'error');
        log('Audio context resume failed: ' + e.message);
      }
    }
    log('Audio capture started (' + audioContext.sampleRate + ' Hz)');
    setMicStatus(audioContext.state === 'running'
      ? 'Microphone active; hold to send'
      : 'Tap Hold to Talk to enable microphone audio',
      audioContext.state === 'running' ? 'active' : '');
  } catch (e) {
    micLevelEl.style.width = '0%';
    micLevelValueEl.textContent = '0%';
    setMicStatus('Microphone unavailable: ' + e.message, 'error');
    log('Mic access failed: ' + e.message);
  } finally {
    audioCaptureStarting = false;
  }
}

function collectAudioFrames(samples) {
  const inputRate = audioContext ? audioContext.sampleRate : CONFIG.audioSampleRate;
  for (let i = 0; i < samples.length; i++) {
    resampleAccumulator += CONFIG.audioSampleRate;
    while (resampleAccumulator >= inputRate) {
      resampleAccumulator -= inputRate;
      captureFrame[captureFrameOffset++] = samples[i];
      if (captureFrameOffset === CONFIG.frameSamples) {
        processCapturedFrame(captureFrame);
        captureFrameOffset = 0;
      }
    }
  }
}

function processCapturedFrame(samples) {
  const int16Samples = new Int16Array(CONFIG.frameSamples);
  for (let i = 0; i < CONFIG.frameSamples; i++) {
    int16Samples[i] = Math.max(-32768, Math.min(32767, samples[i] * 32767));
  }
  updateMicLevel(int16Samples);
  const shouldTransmit =
    (remoteSpyLive && !isMuted) ||
    (isTalking && !isMuted && !isSpyLive && !isSpyClip);
  if (!shouldTransmit && !isRecording && !remoteSpyRecording) return;

  const predictor = adpcmPredictor;
  const index = adpcmIndex;
  const adpcmBytes = encodeADPCM(int16Samples);
  const packet = createAudioPacket(adpcmBytes, predictor, index);
  if (shouldTransmit) sendPacket(packet);
  if (isRecording) recordedFrames.push(packet);
  if (remoteSpyRecording) remoteSpyFrames.push(packet);
}

function updateMicLevel(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const rms = Math.sqrt(sum / samples.length) / 32768;
  const level = Math.min(1, rms * 4);
  micLevelEl.style.width = (level * 100) + '%';
  micLevelValueEl.textContent = Math.round(level * 100) + '%';
}

const stepTable = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767];
const indexTable = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];

function encodeADPCM(samples) {
  const bytes = new Uint8Array(CONFIG.frameBytes);
  let byteIndex = 0, nibble = 0, isFirstNibble = true;
  for (let i = 0; i < samples.length; i++) {
    let diff = samples[i] - adpcmPredictor;
    let step = stepTable[adpcmIndex];
    let code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let tempStep = step;
    if (diff >= tempStep) { code |= 4; diff -= tempStep; }
    tempStep >>= 1;
    if (diff >= tempStep) { code |= 2; diff -= tempStep; }
    tempStep >>= 1;
    if (diff >= tempStep) { code |= 1; }
    let diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    if (code & 8) adpcmPredictor -= diffq; else adpcmPredictor += diffq;
    if (adpcmPredictor > 32767) adpcmPredictor = 32767;
    else if (adpcmPredictor < -32768) adpcmPredictor = -32768;
    adpcmIndex += indexTable[code & 7];
    if (adpcmIndex < 0) adpcmIndex = 0; else if (adpcmIndex > 88) adpcmIndex = 88;
    if (isFirstNibble) {
      bytes[byteIndex] = code << 4;
      isFirstNibble = false;
    } else {
      bytes[byteIndex++] |= code;
      isFirstNibble = true;
    }
  }
  return bytes.buffer;
}

function createAudioPacket(adpcmBytes, predictor, index) {
  const headerSize = 10;
  const payload = new Uint8Array(adpcmBytes);
  const packet = new ArrayBuffer(headerSize + payload.length);
  const view = new DataView(packet);
  const bytes = new Uint8Array(packet);
  view.setUint8(0, 0x49);
  view.setUint8(1, 1);
  view.setUint8(2, 2);
  view.setUint8(3, 0);
  view.setUint16(4, txSequence++, true);
  view.setInt16(6, predictor, true);
  view.setUint8(8, index);
  view.setUint8(9, payload.length);
  bytes.set(payload, headerSize);
  return packet;
}

function decodeADPCM(adpcmBytes, predictor, index) {
  const samples = new Int16Array(CONFIG.frameSamples);
  let p = predictor, idx = index, byteIndex = 0, isFirstNibble = true, currentByte = 0;
  for (let i = 0; i < CONFIG.frameSamples; i++) {
    let code;
    if (isFirstNibble) {
      currentByte = adpcmBytes[byteIndex++];
      code = (currentByte >> 4) & 0x0F;
      isFirstNibble = false;
    } else {
      code = currentByte & 0x0F;
      isFirstNibble = true;
    }
    let step = stepTable[idx];
    let diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    if (code & 8) p -= diffq; else p += diffq;
    if (p > 32767) p = 32767; else if (p < -32768) p = -32768;
    samples[i] = p;
    idx += indexTable[code & 7];
    if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
  }
  return samples;
}

function handleAudioPacket(data) {
  const view = new DataView(data);
  if (data.byteLength < 10 || view.getUint8(0) !== 0x49 ||
      view.getUint8(1) !== 1) return;
  if (view.getUint8(2) !== 2) return;
  const predictor = view.getInt16(6, true);
  const index = view.getUint8(8);
  const payloadLen = view.getUint8(9);
  if (index > 88 || payloadLen === 0 || data.byteLength < 10 + payloadLen) return;
  const adpcmBytes = new Uint8Array(data, 10, payloadLen);
  const samples = decodeADPCM(adpcmBytes, predictor, index);
  updateRxLevel(samples);
  playbackQueue.push(samples);
  playNextFrame();
}

function handleBinaryPacket(data) {
  if (data.byteLength < 10) return;
  const view = new DataView(data);
  if (view.getUint8(0) !== 0x49 || view.getUint8(1) !== 1) return;

  if (view.getUint8(2) === 2) {
    handleAudioPacket(data);
  } else if (view.getUint8(2) === 3 && data.byteLength >= 11 &&
             view.getUint8(9) >= 1) {
    handleSpyPacket(view.getUint8(10));
  }
}

function handleSpyPacket(command) {
  switch (command) {
    case SPY_CMD.LIVE_START:
      if (isTalking || isRecording || isSpyLive || isSpyClip ||
          remoteSpyRecording || isMuted || !micStream) {
        sendPacket(createSpyPacket(SPY_CMD.BUSY));
        return;
      }
      remoteSpyLive = true;
      remoteSpyLiveTimer = setTimeout(stopRemoteSpyLive, 30000);
      updateTxStatus(true);
      log('ESP started spying on this microphone');
      break;
    case SPY_CMD.LIVE_STOP:
      stopRemoteSpyLive();
      updateTxStatus(true);
      log('ESP stopped spying on this microphone');
      break;
    case SPY_CMD.CLIP_START:
      if (isTalking || isRecording || isSpyLive || isSpyClip ||
          remoteSpyLive || isMuted || !micStream) {
        sendPacket(createSpyPacket(SPY_CMD.BUSY));
        return;
      }
      remoteSpyFrames = [];
      remoteSpyRecording = true;
      remoteSpyClipTimer = setTimeout(finishRemoteSpyClip, 15000);
      updateTxStatus(true);
      log('ESP started recording a spy clip');
      break;
    case SPY_CMD.CLIP_STOP:
      finishRemoteSpyClip();
      break;
    case SPY_CMD.CLIP_DONE:
      if (isSpyClip) {
        isSpyClip = false;
        spyTargetPeer = null;
        updateUIForMode();
        log('Spy clip received');
      }
      break;
    case SPY_CMD.BUSY:
      if (isSpyLive || isSpyClip) {
        isSpyLive = false;
        isSpyClip = false;
        spyTargetPeer = null;
        updateUIForMode();
      }
      log('ESP is busy and cannot start the spy request');
      break;
  }
}

function stopRemoteSpyLive() {
  remoteSpyLive = false;
  clearTimeout(remoteSpyLiveTimer);
  remoteSpyLiveTimer = null;
  updateTxStatus(true);
}

function finishRemoteSpyClip() {
  if (!remoteSpyRecording) return;
  remoteSpyRecording = false;
  clearTimeout(remoteSpyClipTimer);
  remoteSpyClipTimer = null;
  updateTxStatus(true);
  sendRemoteSpyClip();
}

async function sendRemoteSpyClip() {
  const frames = remoteSpyFrames;
  remoteSpyFrames = [];
  for (const frame of frames) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    sendPacket(frame);
    await new Promise(resolve => setTimeout(resolve, CONFIG.frameMs));
  }
  if (ws && ws.readyState === WebSocket.OPEN) {
    sendPacket(createSpyPacket(SPY_CMD.CLIP_DONE));
  }
  log('Spy clip sent to ESP');
}

async function sendRecordedClip() {
  const frames = recordedFrames;
  recordedFrames = [];
  for (const frame of frames) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    sendPacket(frame);
    await new Promise(resolve => setTimeout(resolve, CONFIG.frameMs));
  }
  log('Clip sent');
}

function updateRxLevel(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const rms = Math.sqrt(sum / samples.length) / 32768;
  const level = Math.min(1, rms * 4);
  rxLevelEl.style.width = (level * 100) + '%';
}

function playNextFrame() {
  if (!audioContext) return;
  while (playbackQueue.length > 0) {
    const samples = playbackQueue.shift();
    const buffer = audioContext.createBuffer(1, samples.length, CONFIG.audioSampleRate);
    const channelData = buffer.getChannelData(0);
    for (let i = 0; i < samples.length; i++) channelData[i] = samples[i] / 32768;
    const source = audioContext.createBufferSource();
    source.buffer = buffer;
    source.connect(audioContext.destination);
    const startAt = Math.max(
      playbackNextTime,
      audioContext.currentTime + CONFIG.frameMs / 1000
    );
    source.start(startAt);
    playbackNextTime = startAt + buffer.duration;
  }
}

function handleTextMessage(data) {
  try {
    const msg = JSON.parse(data);
    if (msg.type === 'hello') {
      // Peer announcement
      updatePeerList(msg);
    } else if (msg.type === 'peer_list') {
      // Full peer list update
      const previousSelection = selectedPeer;
      peers = normalizePeers(Array.isArray(msg.peers) ? msg.peers : [])
        .filter(peer => !peer.webPeer ||
          (peer.identityMac
            ? peer.identityMac.toUpperCase() !== CONFIG.webPeerIdentity
            : peer.name !== CONFIG.webPeerName));
      if (selectedPeer) {
        selectedPeer = peers.find(peer => samePeer(peer, previousSelection)) || null;
      }
      renderPeerList();
      if (previousSelection && !selectedPeer) {
        peerNameEl.textContent = 'All peers';
        peerRssiEl.textContent = 'Broadcasting';
      }
      if (selectedPeer || previousSelection) sendAudioTarget();
    } else if (msg.type === 'status') {
      log(msg.message);
    } else if (msg.type === 'pong') {
      // connection alive
    } else if (msg.type === 'spy_response') {
      handleSpyResponse(msg);
    }
  } catch (e) {}
}

function updatePeerList(msg) {
  if (!msg.mac || !msg.name) return;
  
  const peerId = msg.id || (msg.mac + '/' + (msg.port || 0));
  const peerData = {
    id: peerId,
    mac: msg.mac,
    port: msg.port || 0,
    viaWifi: Boolean(msg.viaWifi),
    webPeer: Boolean(msg.webPeer),
    name: msg.name,
    rssi: msg.rssi || 0,
    battery: msg.battery,
    mode: msg.mode,
    lastSeen: Date.now()
  };
  const existingIdx = peers.findIndex(peer => samePeer(peer, peerData));

  if (existingIdx >= 0) {
    peers[existingIdx] = {
      ...peers[existingIdx],
      ...peerData,
      id: peerData.id
    };
    peers = normalizePeers(peers);
    if (selectedPeer && samePeer(selectedPeer, peerData)) {
      selectedPeer = peers.find(peer => samePeer(peer, peerData)) || peerData;
    }
  } else {
    peers.push(peerData);

    log('Peer online: ' + peerData.name);
  }
  if (selectedPeer && samePeer(selectedPeer, peerData)) {
    peerNameEl.textContent = peerData.name;
    peerRssiEl.textContent = peerData.rssi + ' dBm';
  }
  renderPeerList();
  if (selectedPeer) sendAudioTarget();
}

function renderPeerList() {
  const items = [{
    peer: { id: 'all', name: 'All peers' },
    selected: !selectedPeer,
    details: 'Broadcast to every peer',
    trailingText: ''
  }, ...peers.map(peer => ({
    peer,
    selected: Boolean(selectedPeer && samePeer(selectedPeer, peer)),
    details: peer.rssi + ' dBm' +
      (peer.battery !== undefined && peer.battery < 101
        ? ' | Bat: ' + peer.battery + '%'
        : '') +
      (peer.webPeer ? ' | Web peer' : ''),
    trailingText: peer.rssi + ' dBm'
  }))];
  const existingItems = new Map(
    Array.from(peerItemsEl.children, item => [item.dataset.peerId, item])
  );
  const desiredIds = new Set(items.map(item => item.peer.id));

  existingItems.forEach((item, id) => {
    if (!desiredIds.has(id)) item.remove();
  });

  let position = peerItemsEl.firstElementChild;
  items.forEach(({ peer, selected, details, trailingText }) => {
    let item = existingItems.get(peer.id);
    if (!item) item = createPeerItem(peer);
    updatePeerItem(item, peer, selected, details, trailingText);
    if (item !== position) peerItemsEl.insertBefore(item, position);
    position = item.nextElementSibling;
  });
}

function createPeerItem(peer) {
  const item = document.createElement('button');
  item.type = 'button';
  item.className = 'peer-item';
  const info = document.createElement('span');
  info.className = 'peer-item-info';
  const name = document.createElement('span');
  name.className = 'peer-item-name';
  name.textContent = peer.name;
  const detail = document.createElement('span');
  detail.className = 'peer-item-details';
  info.append(name, detail);
  item.appendChild(info);
  const trailing = document.createElement('span');
  trailing.className = 'peer-item-rssi';
  item.appendChild(trailing);
  updatePeerItem(item, peer, false, '', '');
  return item;
}

function updatePeerItem(item, peer, selected, details, trailingText) {
  item.dataset.peerId = peer.id;
  item.classList.toggle('selected', selected);
  item.setAttribute('aria-pressed', String(selected));
  item.querySelector('.peer-item-name').textContent = peer.name;
  item.querySelector('.peer-item-details').textContent = details;
  item.querySelector('.peer-item-rssi').textContent = trailingText;
}

peerItemsEl.addEventListener('click', event => {
  const item = event.target.closest('button[data-peer-id]');
  if (item && peerItemsEl.contains(item)) selectPeer(item.dataset.peerId);
});

function selectPeer(id) {
  if (id === 'all') {
    selectedPeer = null;
    peerNameEl.textContent = 'All peers';
    peerRssiEl.textContent = 'Broadcasting';
    renderPeerList();
    sendAudioTarget();
    log('Selected all peers');
    return;
  }
  selectedPeer = peers.find(p => p.id === id) || null;
  if (selectedPeer) {
    peerNameEl.textContent = selectedPeer.name;
    peerRssiEl.textContent = selectedPeer.rssi + ' dBm';
    renderPeerList();
    sendAudioTarget();
    log('Selected peer: ' + selectedPeer.name);
  }
}

function handleSpyResponse(msg) {
  if (msg.success) {
    log('Spy command success: ' + msg.command);
  } else {
    log('Spy command failed: ' + (msg.error || 'unknown'));
    stopSpy();
  }
}

function updateUIForMode() {
  const isSpyMode = currentMode === 'spy';
  modeToggleEl.querySelectorAll('button[data-mode]').forEach(button => {
    const active = button.dataset.mode === currentMode;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  });
  
  // Show/hide controls based on mode
  talkBtn.style.display = isSpyMode ? 'none' : 'flex';
  clipBtn.style.display = isSpyMode ? 'none' : 'flex';
  spyControlsEl.classList.toggle('hidden', !isSpyMode);
  
  // Update peer list visibility
  if (isSpyMode) {
    peerListEl.classList.remove('hidden');
  }
  
  // Update spy button states
  spyLiveBtn.classList.toggle('active', isSpyLive);
  spyClipBtn.classList.toggle('active', isSpyClip);
  spyStopBtn.classList.toggle('hidden', !isSpyLive && !isSpyClip);
  
  // Update button text
  if (isSpyLive) {
    spyLiveBtn.textContent = 'Spying Live...';
  } else {
    spyLiveBtn.textContent = 'Spy Live';
  }
  
  if (isSpyClip) {
    spyClipBtn.textContent = 'Recording Clip...';
  } else {
    spyClipBtn.textContent = 'Spy Clip';
  }
}

function startSpyLive() {
  if (!selectedPeer) {
    log('No peer selected for spy');
    return;
  }
  if (isSpyClip) return;
  
  isSpyLive = true;
  spyTargetPeer = selectedPeer;
  sendSpyCommand(SPY_CMD.LIVE_START, selectedPeer);
  updateUIForMode();
  log('Spy live started on ' + selectedPeer.name);
}

function stopSpyLive() {
  if (!isSpyLive) return;
  
  if (spyTargetPeer) {
    sendSpyCommand(SPY_CMD.LIVE_STOP, spyTargetPeer);
  }
  isSpyLive = false;
  spyTargetPeer = null;
  updateUIForMode();
  log('Spy live stopped');
}

function startSpyClip() {
  if (!selectedPeer) {
    log('No peer selected for spy');
    return;
  }
  if (isSpyLive) return;
  
  isSpyClip = true;
  spyTargetPeer = selectedPeer;
  recordedFrames = [];
  sendSpyCommand(SPY_CMD.CLIP_START, selectedPeer);
  updateUIForMode();
  log('Spy clip recording started on ' + selectedPeer.name);
}

function stopSpyClip() {
  if (!isSpyClip) return;
  
  if (spyTargetPeer) {
    sendSpyCommand(SPY_CMD.CLIP_STOP, spyTargetPeer);
  }
  isSpyClip = false;
  spyTargetPeer = null;
  updateUIForMode();
  log('Spy clip stopped, waiting for clip...');
}

function stopSpy() {
  if (isSpyLive) stopSpyLive();
  if (isSpyClip) stopSpyClip();
}

// Event handlers
function stopTalking() {
  if (!isTalking) return;
  isTalking = false;
  talkBtn.classList.remove('active');
  talkBtn.textContent = 'Hold to Talk';
  updateTxStatus(true);
}

talkBtn.addEventListener('pointerdown', (event) => {
  event.preventDefault();
  if (isMuted) return;
  void startAudioCapture(true);
  isTalking = true;
  talkBtn.classList.add('active');
  talkBtn.textContent = 'Talking...';
  updateTxStatus(true);
  if (audioContext && audioContext.state === 'suspended') {
    audioContext.resume().catch(e => log('Audio context resume failed: ' + e.message));
  }
});
window.addEventListener('pointerup', stopTalking);
window.addEventListener('pointercancel', stopTalking);

clipBtn.addEventListener('click', () => {
  if (isRecording) {
    isRecording = false;
    clipBtn.textContent = 'Record Clip';
    clipBtn.classList.remove('recording');
    updateTxStatus(true);
    sendRecordedClip();
  } else {
    void startAudioCapture(true);
    isRecording = true;
    recordedFrames = [];
    clipBtn.textContent = 'Stop & Send';
    clipBtn.classList.add('recording');
    updateTxStatus(true);
    log('Recording clip...');
  }
});

muteBtn.addEventListener('click', () => {
  isMuted = !isMuted;
  muteBtn.classList.toggle('muted', isMuted);
  muteBtn.textContent = isMuted ? 'Unmute' : 'Mute';
  if (isMuted) stopTalking();
  updateTxStatus(true);
});

modeToggleEl.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-mode]');
  if (!button || !modeToggleEl.contains(button)) return;
  currentMode = button.dataset.mode;
  stopSpy(); // Stop any active spy when changing mode
  updateUIForMode();
  log('Mode: ' + currentMode);
});

volumeSlider.addEventListener('input', () => {
  const gain = Number(volumeSlider.value) / 100;
  if (micGainNode) micGainNode.gain.setTargetAtTime(
    gain,
    audioContext.currentTime,
    0.015
  );
  micGainValueEl.textContent = volumeSlider.value + '%';
});

spyLiveBtn.addEventListener('click', () => {
  if (isSpyLive) {
    stopSpyLive();
  } else {
    startSpyLive();
  }
});

spyClipBtn.addEventListener('click', () => {
  if (isSpyClip) {
    stopSpyClip();
  } else {
    startSpyClip();
  }
});

spyStopBtn.addEventListener('click', () => {
  stopSpy();
});

renderPeerList();
window.addEventListener('load', () => { connectWebSocket(); });

document.addEventListener('visibilitychange', () => {
  if (!document.hidden && (!ws || ws.readyState !== WebSocket.OPEN)) connectWebSocket();
});

window.addEventListener('online', () => {
  if (!ws || ws.readyState !== WebSocket.OPEN) connectWebSocket();
});

window.addEventListener('offline', () => { log('Network offline'); });

// Keep the browser visible to the firmware's five-second peer timeout.
setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN) {
    sendHello();
  }
}, 2000);