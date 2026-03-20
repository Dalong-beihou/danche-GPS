const SpeedUtils = (() => {
  function normalizeSpeed({ incomingSpeed, prevSpeed, prevSpeedTs, online, now, holdMs }) {
    if (!online) {
      return { speed: null, speedTs: prevSpeedTs || 0 };
    }
    const parsed = parseFloat(incomingSpeed);
    const hasIncoming = Number.isFinite(parsed);
    const prevValid = Number.isFinite(prevSpeed);
    let nextSpeed = prevValid ? prevSpeed : null;
    let nextTs = prevSpeedTs || 0;
    if (hasIncoming) {
      if (parsed === 0 && prevValid && prevSpeed > 0 && now - nextTs < holdMs) {
        nextSpeed = prevSpeed;
      } else {
        nextSpeed = parsed;
        nextTs = now;
      }
    }
    return { speed: nextSpeed, speedTs: nextTs };
  }

  function easeOutCubic(t) {
    const inv = 1 - t;
    return 1 - inv * inv * inv;
  }

  function lerp(a, b, t) {
    return a + (b - a) * t;
  }

  function interpolateSpeed(from, to, progress) {
    return lerp(from, to, easeOutCubic(progress));
  }

  return {
    normalizeSpeed,
    interpolateSpeed
  };
})();

if (typeof window !== 'undefined') {
  window.SpeedUtils = SpeedUtils;
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = SpeedUtils;
}
