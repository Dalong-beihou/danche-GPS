const TrackUtils = (() => {
  function buildClearPlan(targetIds, trackPolylines) {
    const ids = Array.isArray(targetIds) ? targetIds : [];
    const polylines = [];
    const uniqueIds = [];
    ids.forEach((id) => {
      if (id && trackPolylines && trackPolylines[id]) {
        uniqueIds.push(id);
        const items = Array.isArray(trackPolylines[id]) ? trackPolylines[id] : [trackPolylines[id]];
        items.forEach((pl) => polylines.push({ id, polyline: pl }));
      }
    });
    return { ids: uniqueIds, polylines };
  }

  return {
    buildClearPlan
  };
})();

if (typeof window !== 'undefined') {
  window.TrackUtils = TrackUtils;
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = TrackUtils;
}
