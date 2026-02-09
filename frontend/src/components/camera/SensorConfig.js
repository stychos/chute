// framesize enum values from esp32-camera v2.1.4 sensor.h
export const SENSORS = {
  OV2640: {
    pid: 0x2640,
    resolutions: [
      { value: 1, label: 'QQVGA (160x120)' },
      { value: 4, label: 'HQVGA (240x176)' },
      { value: 6, label: 'QVGA (320x240)' },
      { value: 8, label: 'CIF (400x296)' },
      { value: 9, label: 'HVGA (480x320)' },
      { value: 10, label: 'VGA (640x480)' },
      { value: 11, label: 'SVGA (800x600)' },
      { value: 12, label: 'XGA (1024x768)' },
      { value: 14, label: 'SXGA (1280x1024)' },
      { value: 15, label: 'UXGA (1600x1200)' },
    ],
    hasSharpness: false,
    maxAgcGain: 30,
  },
  OV3660: {
    pid: 0x3660,
    resolutions: [
      { value: 1, label: 'QQVGA (160x120)' },
      { value: 6, label: 'QVGA (320x240)' },
      { value: 10, label: 'VGA (640x480)' },
      { value: 11, label: 'SVGA (800x600)' },
      { value: 12, label: 'XGA (1024x768)' },
      { value: 14, label: 'SXGA (1280x1024)' },
      { value: 15, label: 'UXGA (1600x1200)' },
      { value: 19, label: 'QXGA (2048x1536)' },
    ],
    hasSharpness: true,
    maxAgcGain: 64,
  },
  OV5640: {
    pid: 0x5640,
    resolutions: [
      { value: 1, label: 'QQVGA (160x120)' },
      { value: 6, label: 'QVGA (320x240)' },
      { value: 10, label: 'VGA (640x480)' },
      { value: 11, label: 'SVGA (800x600)' },
      { value: 12, label: 'XGA (1024x768)' },
      { value: 14, label: 'SXGA (1280x1024)' },
      { value: 15, label: 'UXGA (1600x1200)' },
      { value: 19, label: 'QXGA (2048x1536)' },
    ],
    hasSharpness: true,
    maxAgcGain: 64,
  },
}

export const EFFECTS = [
  { value: 0, label: 'Normal' },
  { value: 1, label: 'Negative' },
  { value: 2, label: 'Grayscale' },
  { value: 3, label: 'Red Tint' },
  { value: 4, label: 'Green Tint' },
  { value: 5, label: 'Blue Tint' },
  { value: 6, label: 'Sepia' },
]

export const WB_MODES = [
  { value: 0, label: 'Auto' },
  { value: 1, label: 'Sunny' },
  { value: 2, label: 'Cloudy' },
  { value: 3, label: 'Office' },
  { value: 4, label: 'Home' },
]

export function getSensorConfig(pid) {
  for (const [name, cfg] of Object.entries(SENSORS)) {
    if (cfg.pid === pid) return { name, ...cfg }
  }
  return { name: 'Unknown', ...SENSORS.OV2640 }
}
