export function fitVideoSize(videoWidth, videoHeight, viewportWidth, viewportHeight) {
  if (![videoWidth, videoHeight, viewportWidth, viewportHeight]
      .every(value => Number.isFinite(value) && value > 0)) {
    throw new RangeError('视频和视口尺寸必须是正数');
  }
  const scale = Math.min(viewportWidth / videoWidth, viewportHeight / videoHeight);
  return {
    width: Math.max(1, Math.floor(videoWidth * scale)),
    height: Math.max(1, Math.floor(videoHeight * scale)),
  };
}

export function cursorViewportPosition(normalizedX, normalizedY, rectangle) {
  const x = Math.max(0, Math.min(65535, normalizedX));
  const y = Math.max(0, Math.min(65535, normalizedY));
  return {
    left: rectangle.left + x / 65535 * rectangle.width,
    top: rectangle.top + y / 65535 * rectangle.height,
  };
}
