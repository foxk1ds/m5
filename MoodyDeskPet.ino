/**
 * ============================================================================
 * Project:       Moody Desk Pet (Interactive Pixel-Art Tamagotchi)
 * Target Board:  M5StickC Plus 1.1 (ESP32-PICO, ST7789v2 135x240, AXP192)
 * Peripherals:   MPU6886 IMU, SPM1423 PDM Mic, Piezo Buzzer, TFT LCD
 * Dependencies:  M5StickCPlus library (Standard)
 * Architecture:  Fully non-blocking state machine, I2S PDM audio DMA,
 *                TFT_eSprite double-buffered pixel rendering.
 * ============================================================================
 */

#include <M5StickCPlus.h>
#include <driver/i2s.h>
#include <math.h>

// ============================================================================
// 1. CONFIGURATION & SENSITIVITY THRESHOLDS (Easily Tunable)
// ============================================================================

// --- Display Configuration ---
#define SCREEN_ROTATION           3       // 3 = Landscape (240x135, USB on right)
#define SCREEN_WIDTH              240
#define SCREEN_HEIGHT             135
#define BRIGHTNESS_AWAKE          12      // AXP192 LDO level (range 7 - 15)
#define BRIGHTNESS_SLEEP          8       // Dimmed state for power saving

// --- Motion & IMU Thresholds ---
#define SLEEP_ENTER_TIME_MS       5000    // Flat & quiet duration to enter sleep
#define FLAT_Z_MIN                0.85f   // Flat desk: Z ~ 1.0g (+/- tilt margin)
#define FLAT_XY_MAX               0.25f   // X and Y should be close to 0g when flat
#define FLAT_GYRO_MAX             15.0f   // Max deg/sec allowed while "at rest"

#define DIZZY_GYRO_THRESHOLD      250.0f  // Sustained angular velocity (deg/sec)
#define DIZZY_TRIGGER_TIME_MS     1400    // Continuous spinning time to trigger
#define DIZZY_DURATION_MS         3000    // Dizziness recovery duration

#define SHAKE_ACCEL_HIGH          1.35f   // Acceleration magnitude peak
#define SHAKE_ACCEL_LOW           0.65f   // Acceleration magnitude trough
#define SHAKE_COUNT_TRIGGER       4       // Alternating oscillations needed
#define SHAKE_WINDOW_MS           1200    // Time window for shake oscillations
#define HAPPY_DURATION_MS         3200    // Time pet stays happy

// --- Acoustic SPM1423 PDM Microphone Settings ---
#define MIC_I2S_PORT              I2S_NUM_0
#define MIC_PIN_CLK               0       // GPIO 0
#define MIC_PIN_DATA              34      // GPIO 34
#define MIC_SAMPLE_RATE           22050
#define MIC_BUFFER_SIZE           128     // Samples per non-blocking DMA chunk
#define MIC_SPIKE_PEAK_TO_PEAK    11000   // Peak-to-peak amplitude for loud sound
#define SURPRISED_DURATION_MS     2200    // Duration for surprised/wake state

// --- Animation & Timer Intervals ---
#define FRAME_INTERVAL_MS         33      // ~30 FPS render loop
#define BLINK_INTERVAL_MIN_MS     2500    // Random blink interval minimum
#define BLINK_INTERVAL_MAX_MS     6000    // Random blink interval maximum
#define BLINK_DURATION_MS         140     // Eye closed duration during blink
#define SNORE_INTERVAL_MS         3500    // Time between snore chirps

// --- Color Palette (16-bit RGB565) ---
#define COLOR_BG                  0x0841  // Deep Midnight Navy
#define COLOR_FACE_BODY           0x1925  // Retro Screen Frame
#define COLOR_EYE_CYAN            0x07FF  // Vibrant Cyber Cyan
#define COLOR_EYE_WHITE           0xFFFF  // Pure White
#define COLOR_CHEEK_PINK          0xF996  // Soft Blushing Pink
#define COLOR_MOUTH_DARK          0x0000  // Shadow / Mouth Black
#define COLOR_HEART_RED           0xF888  // Soft Pixel Crimson
#define COLOR_ZZZ_BLUE            0x7DFF  // Dreamy Cloud Blue
#define COLOR_EXCLAMATION_YELLOW  0xFFE0  // Alert Yellow
#define COLOR_BATTERY_GREEN       0x07E0  // Green battery bar

// ============================================================================
// 2. STATE MACHINE ENUMERATION & VARIABLES
// ============================================================================

enum PetState {
  STATE_IDLE,
  STATE_SLEEPING,
  STATE_DIZZY,
  STATE_HAPPY,
  STATE_SURPRISED
};

PetState currentState    = STATE_IDLE;
PetState previousState   = STATE_IDLE;

// State Timers
uint32_t stateTimer      = 0;
uint32_t lastFlatTime    = 0;
uint32_t dizzyStartTime  = 0;
uint32_t lastFrameTime   = 0;

// Shake detection state variables
int      shakeCounter    = 0;
bool     shakeHighMet    = false;
uint32_t lastShakeTime   = 0;

// Eye Blinking & Glancing Logic
uint32_t nextBlinkTime   = 3000;
uint32_t blinkStartTime  = 0;
bool     isBlinking      = false;
int8_t   eyeGlanceX      = 0;    // -3 (left), 0 (center), +3 (right)
uint32_t nextGlanceTime  = 4000;

// Idle breathing bobbing animation
uint8_t  idleBobOffset   = 0;
uint32_t nextIdleBobTime = 0;

// Audio Configuration & Non-Blocking Melody Player
bool     isMuted         = false;
uint32_t nextSnoreTime   = 0;
int      soundSeqStep    = 0;
uint32_t soundSeqTimer   = 0;
uint8_t  activeSound     = 0;    // 0: none, 1: happy, 2: surprised, 3: snore

// Floating Particles
struct FloatingParticle {
  float x;
  float y;
  float speedY;
  float phase;
  bool  active;
  uint8_t type; // 0 = Zzz, 1 = Heart
};
#define MAX_PARTICLES 5
FloatingParticle particles[MAX_PARTICLES];
uint32_t nextParticleTime = 0;

// Double Buffer Canvas Sprite
TFT_eSprite canvas = TFT_eSprite(&M5.Lcd);

// ============================================================================
// 3. HARDWARE SUBSYSTEM INITIALIZATION
// ============================================================================

/**
 * Configure I2S DMA for the SPM1423 PDM digital microphone.
 * Uses 22.05 kHz sample rate, 16-bit mono right channel.
 */
void initMicrophone() {
  i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate          = MIC_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ALL_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 2,
    .dma_buf_len          = MIC_BUFFER_SIZE,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num           = I2S_PIN_NO_CHANGE,
    .ws_io_num            = MIC_PIN_CLK,
    .data_out_num         = I2S_PIN_NO_CHANGE,
    .data_in_num          = MIC_PIN_DATA
  };

  i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(MIC_I2S_PORT, &pin_config);
  i2s_set_clk(MIC_I2S_PORT, MIC_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

/**
 * Non-blocking microphone read.
 * Returns peak-to-peak amplitude across the buffer, effectively
 * eliminating PDM DC offset without computational overhead.
 */
int32_t getMicAmplitude() {
  int16_t sampleBuffer[MIC_BUFFER_SIZE];
  size_t bytesRead = 0;

  // 0 wait timeout ensures this call never blocks the animation loop
  esp_err_t result = i2s_read(MIC_I2S_PORT, (char*)sampleBuffer, sizeof(sampleBuffer), &bytesRead, 0);

  if (result == ESP_OK && bytesRead > 0) {
    int samples = bytesRead / sizeof(int16_t);
    int16_t minVal = 32767;
    int16_t maxVal = -32768;

    for (int i = 0; i < samples; i++) {
      if (sampleBuffer[i] < minVal) minVal = sampleBuffer[i];
      if (sampleBuffer[i] > maxVal) maxVal = sampleBuffer[i];
    }
    return (int32_t)(maxVal - minVal);
  }
  return 0;
}

// ============================================================================
// 4. NON-BLOCKING AUDIO ENGINE
// ============================================================================

void triggerSound(uint8_t soundType) {
  if (isMuted) return;
  activeSound   = soundType;
  soundSeqStep  = 0;
  soundSeqTimer = millis();
}

void updateAudio(uint32_t now) {
  if (isMuted) {
    M5.Beep.mute();
    activeSound = 0;
    return;
  }

  if (activeSound == 1) { // Happy: Ascending two-tone chirp
    if (soundSeqStep == 0) {
      M5.Beep.tone(880); // A5
      if (now - soundSeqTimer >= 90) {
        soundSeqStep = 1;
        soundSeqTimer = now;
      }
    } else if (soundSeqStep == 1) {
      M5.Beep.tone(1320); // E6
      if (now - soundSeqTimer >= 140) {
        M5.Beep.mute();
        activeSound = 0;
      }
    }
  } else if (activeSound == 2) { // Surprised: High warning squeak
    if (soundSeqStep == 0) {
      M5.Beep.tone(2400);
      if (now - soundSeqTimer >= 160) {
        M5.Beep.mute();
        activeSound = 0;
      }
    }
  } else if (activeSound == 3) { // Snore: Rhythmic deep chirp
    if (soundSeqStep == 0) {
      M5.Beep.tone(160);
      if (now - soundSeqTimer >= 40) {
        M5.Beep.mute();
        activeSound = 0;
      }
    }
  }
}

// ============================================================================
// 5. PARTICLE SYSTEM HELPERS
// ============================================================================

void resetParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    particles[i].active = false;
  }
}

void spawnParticle(uint8_t type, float startX, float startY) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (!particles[i].active) {
      particles[i].active = true;
      particles[i].type   = type;
      particles[i].x      = startX + (float)random(-12, 12);
      particles[i].y      = startY;
      particles[i].speedY = (type == 0) ? -0.45f : -0.75f; // Zzz drifts gentler
      particles[i].phase  = (float)random(0, 360) * 0.01745f;
      break;
    }
  }
}

void updateParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (particles[i].active) {
      particles[i].y += particles[i].speedY;
      particles[i].phase += 0.08f;
      particles[i].x += sinf(particles[i].phase) * 0.45f; // Gentle sway

      if (particles[i].y < 10) {
        particles[i].active = false;
      }
    }
  }
}

// ============================================================================
// 6. PIXEL-ART PRIMITIVES & DRAWING ROUTINES
// ============================================================================

void drawPixelHeart(int x, int y, uint16_t color) {
  canvas.fillRect(x + 1, y,     2, 1, color);
  canvas.fillRect(x + 4, y,     2, 1, color);
  canvas.fillRect(x,     y + 1, 7, 2, color);
  canvas.fillRect(x + 1, y + 3, 5, 1, color);
  canvas.fillRect(x + 2, y + 4, 3, 1, color);
  canvas.fillRect(x + 3, y + 5, 1, 1, color);
}

void drawPixelZ(int x, int y, uint8_t scale, uint16_t color) {
  int w = 5 * scale;
  int h = 6 * scale;
  canvas.fillRect(x, y, w, scale, color);
  canvas.fillRect(x + w - scale * 2, y + scale, scale, scale, color);
  canvas.fillRect(x + scale * 2, y + scale * 2, scale, scale, color);
  canvas.fillRect(x + scale, y + scale * 3, scale, scale, color);
  canvas.fillRect(x, y + h - scale, w, scale, color);
}

void drawPixelExclamation(int x, int y, uint16_t color) {
  canvas.fillRect(x,     y,     4, 10, color);
  canvas.fillRect(x + 1, y + 10, 2, 2, color);
  canvas.fillRect(x + 1, y + 14, 2, 3, color);
}

void drawDizzyEye(int cx, int cy, uint16_t color) {
  for (int d = -6; d <= 6; d++) {
    canvas.fillRect(cx + d - 1, cy + d - 1, 2, 2, color);
    canvas.fillRect(cx + d - 1, cy - d - 1, 2, 2, color);
  }
}

void drawHappyEye(int cx, int cy, uint16_t color) {
  canvas.fillRect(cx - 9, cy + 3, 4, 3, color);
  canvas.fillRect(cx - 5, cy,     4, 3, color);
  canvas.fillRect(cx - 1, cy - 2, 3, 3, color);
  canvas.fillRect(cx + 2, cy,     4, 3, color);
  canvas.fillRect(cx + 6, cy + 3, 4, 3, color);
}

void drawOpenEye(int cx, int cy, int glanceX, bool startled, uint16_t outerColor, uint16_t pupilColor) {
  int eyeW = startled ? 20 : 18;
  int eyeH = startled ? 22 : 18;

  // Eye Sclera / Base Shape
  canvas.fillRoundRect(cx - eyeW / 2, cy - eyeH / 2, eyeW, eyeH, 5, outerColor);

  // Inner Dark Iris / Pupil
  int pupilW = startled ? 8 : 10;
  int pupilH = startled ? 8 : 10;
  int px = cx - pupilW / 2 + glanceX;
  int py = cy - pupilH / 2;
  canvas.fillRect(px, py, pupilW, pupilH, pupilColor);

  // Pixel Catchlight / Glint
  canvas.fillRect(px + 1, py + 1, 3, 3, COLOR_EYE_WHITE);
}

void drawClosedEye(int cx, int cy, uint16_t color) {
  canvas.fillRect(cx - 8, cy, 16, 3, color);
  canvas.fillRect(cx - 7, cy + 3, 3, 2, color);
  canvas.fillRect(cx + 4, cy + 3, 3, 2, color);
}

void drawMouth(int cx, int cy, PetState state, uint32_t now) {
  switch (state) {
    case STATE_IDLE:
      // Neutral cute cat-mouth ':3' style
      canvas.fillRect(cx - 5, cy, 3, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx - 2, cy + 2, 4, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx + 2, cy, 3, 2, COLOR_MOUTH_DARK);
      break;

    case STATE_SLEEPING:
      // Subtle relaxed dot mouth
      canvas.fillRect(cx - 2, cy + 1, 4, 2, COLOR_MOUTH_DARK);
      break;

    case STATE_HAPPY:
      // Open grinning curved smile
      canvas.fillRect(cx - 8, cy, 16, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx - 6, cy + 2, 12, 3, COLOR_MOUTH_DARK);
      canvas.fillRect(cx - 3, cy + 5, 6, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx - 3, cy + 3, 6, 2, COLOR_HEART_RED); // Tongue
      break;

    case STATE_DIZZY:
      // Wavy squiggly confused mouth
      canvas.fillRect(cx - 8, cy + (sinf(now * 0.015f) > 0 ? 0 : 3), 4, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx - 4, cy + 2, 4, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx,     cy + (sinf(now * 0.015f) > 0 ? 3 : 0), 4, 2, COLOR_MOUTH_DARK);
      canvas.fillRect(cx + 4, cy + 2, 4, 2, COLOR_MOUTH_DARK);
      break;

    case STATE_SURPRISED:
      // Shocked small vertical 'O' mouth
      canvas.fillRoundRect(cx - 4, cy, 8, 10, 2, COLOR_MOUTH_DARK);
      break;
  }
}

void drawCheeks(int leftX, int rightX, int y) {
  canvas.fillRect(leftX  - 3, y, 7, 3, COLOR_CHEEK_PINK);
  canvas.fillRect(rightX - 3, y, 7, 3, COLOR_CHEEK_PINK);
}

void drawStatusBar() {
  // Top-left status icon: Muted/Unmuted indicator
  if (isMuted) {
    canvas.drawString("[MUTED]", 6, 6);
  } else {
    canvas.drawString("[AUDIO]", 6, 6);
  }

  // Battery percentage / voltage display
  float vbat = M5.Axp.GetBatVoltage();
  int batPercent = (int)((vbat - 3.2f) / (4.15f - 3.2f) * 100.0f);
  if (batPercent > 100) batPercent = 100;
  if (batPercent < 0) batPercent = 0;

  // Mini Battery Icon (Top-Right)
  int bx = SCREEN_WIDTH - 38;
  int by = 6;
  canvas.drawRect(bx, by, 28, 10, COLOR_EYE_WHITE);
  canvas.fillRect(bx + 28, by + 2, 2, 6, COLOR_EYE_WHITE);
  int fillW = (batPercent * 24) / 100;
  if (fillW > 0) {
    canvas.fillRect(bx + 2, by + 2, fillW, 6, (batPercent < 20) ? COLOR_HEART_RED : COLOR_BATTERY_GREEN);
  }
}

// ============================================================================
// 7. MAIN RENDER FUNCTION (FLICKER-FREE CANVAS)
// ============================================================================

void renderPet(uint32_t now) {
  canvas.fillSprite(COLOR_BG);

  // Bobbing animation offset (simulating idle breathing or happy hops)
  int bobY = 0;
  if (currentState == STATE_IDLE) {
    bobY = idleBobOffset; // 0 or 2 pixels
  } else if (currentState == STATE_HAPPY) {
    bobY = (int)(abs(sinf(now * 0.012f)) * -7.0f); // Bouncy hop
  }

  int centerX = SCREEN_WIDTH / 2;
  int centerY = (SCREEN_HEIGHT / 2) + 6 + bobY;

  int leftEyeX  = centerX - 32;
  int rightEyeX = centerX + 32;
  int eyeY      = centerY - 10;
  int cheekY    = centerY + 12;
  int mouthY    = centerY + 10;

  // --- Render Eyes Based on State ---
  switch (currentState) {
    case STATE_IDLE:
      if (isBlinking) {
        drawClosedEye(leftEyeX,  eyeY, COLOR_EYE_CYAN);
        drawClosedEye(rightEyeX, eyeY, COLOR_EYE_CYAN);
      } else {
        drawOpenEye(leftEyeX,  eyeY, eyeGlanceX, false, COLOR_EYE_CYAN, COLOR_FACE_BODY);
        drawOpenEye(rightEyeX, eyeY, eyeGlanceX, false, COLOR_EYE_CYAN, COLOR_FACE_BODY);
      }
      drawCheeks(leftEyeX, rightEyeX, cheekY);
      break;

    case STATE_SLEEPING:
      drawClosedEye(leftEyeX,  eyeY, COLOR_ZZZ_BLUE);
      drawClosedEye(rightEyeX, eyeY, COLOR_ZZZ_BLUE);
      break;

    case STATE_HAPPY:
      drawHappyEye(leftEyeX,  eyeY, COLOR_EYE_CYAN);
      drawHappyEye(rightEyeX, eyeY, COLOR_EYE_CYAN);
      drawCheeks(leftEyeX, rightEyeX, cheekY);
      break;

    case STATE_DIZZY:
      drawDizzyEye(leftEyeX,  eyeY, COLOR_EXCLAMATION_YELLOW);
      drawDizzyEye(rightEyeX, eyeY, COLOR_EXCLAMATION_YELLOW);
      break;

    case STATE_SURPRISED:
      drawOpenEye(leftEyeX,  eyeY, 0, true, COLOR_EYE_WHITE, COLOR_MOUTH_DARK);
      drawOpenEye(rightEyeX, eyeY, 0, true, COLOR_EYE_WHITE, COLOR_MOUTH_DARK);
      drawPixelExclamation(centerX - 2, centerY - 46, COLOR_EXCLAMATION_YELLOW);
      break;
  }

  // --- Render Mouth ---
  drawMouth(centerX, mouthY, currentState, now);

  // --- Render Particles (Floating Hearts & Zzz's) ---
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (particles[i].active) {
      if (particles[i].type == 0) { // Zzz
        uint8_t scale = (particles[i].y < 35) ? 2 : 1;
        drawPixelZ((int)particles[i].x, (int)particles[i].y, scale, COLOR_ZZZ_BLUE);
      } else { // Heart
        drawPixelHeart((int)particles[i].x, (int)particles[i].y, COLOR_HEART_RED);
      }
    }
  }

  // --- Render Top Status Bar ---
  drawStatusBar();

  // Push Canvas to Display (Zero Flicker)
  canvas.pushSprite(0, 0);
}

// ============================================================================
// 8. TRANSITIONS & STATE MACHINE HELPERS
// ============================================================================

void changeState(PetState newState) {
  if (currentState == newState) return;

  previousState = currentState;
  currentState  = newState;
  stateTimer    = millis();

  // Manage Screen Brightness
  if (newState == STATE_SLEEPING) {
    M5.Axp.ScreenBreath(BRIGHTNESS_SLEEP);
    resetParticles();
    nextSnoreTime = millis() + 2000;
  } else {
    M5.Axp.ScreenBreath(BRIGHTNESS_AWAKE);
  }

  // Trigger state-specific audio effects
  if (newState == STATE_HAPPY) {
    triggerSound(1);
    resetParticles();
  } else if (newState == STATE_SURPRISED) {
    triggerSound(2);
  }
}

// ============================================================================
// 9. ARDUINO SETUP & MAIN LOOP
// ============================================================================

void setup() {
  // Initialize M5StickC Plus (LCD, Power, Serial enabled)
  M5.begin(true, true, true);
  M5.Lcd.setRotation(SCREEN_ROTATION);
  M5.Axp.ScreenBreath(BRIGHTNESS_AWAKE);

  // Create Double-Buffered Canvas Sprite (Full Screen 240x135)
  canvas.setColorDepth(16);
  canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextColor(COLOR_EYE_WHITE, COLOR_BG);

  // Initialize Sensors
  M5.Imu.Init();
  initMicrophone();

  // Seed random generator with analog noise from unconnected GPIO36 (ADC0)
  randomSeed(analogRead(36));

  lastFlatTime   = millis();
  stateTimer     = millis();
  lastFrameTime  = millis();
  nextBlinkTime  = millis() + random(BLINK_INTERVAL_MIN_MS, BLINK_INTERVAL_MAX_MS);
  nextGlanceTime = millis() + 4000;
}

void loop() {
  uint32_t now = millis();

  // 1. Process Hardware Button Events
  M5.update();

  // Button A (M5 Front Button): Pet / Cycle Expressions
  if (M5.BtnA.wasPressed()) {
    if (currentState == STATE_SLEEPING) {
      changeState(STATE_SURPRISED);
    } else if (currentState == STATE_IDLE) {
      changeState(STATE_HAPPY);
    } else {
      changeState(STATE_IDLE);
    }
  }

  // Button B (Side Button): Toggle Audio Mute
  if (M5.BtnB.wasPressed()) {
    isMuted = !isMuted;
    if (isMuted) {
      M5.Beep.mute();
    } else {
      triggerSound(1); // Confirmation chirp
    }
  }

  // 2. Read IMU Data
  float ax = 0, ay = 0, az = 0;
  float gx = 0, gy = 0, gz = 0;
  M5.Imu.getAccelData(&ax, &ay, &az);
  M5.Imu.getGyroData(&gx, &gy, &gz);

  float accelMagnitude = sqrtf(ax * ax + ay * ay + az * az);
  float gyroMagnitude  = sqrtf(gx * gx + gy * gy + gz * gz);

  // 3. Read PDM Microphone Data (Non-blocking)
  int32_t micAmplitude = getMicAmplitude();

  // 4. State Triggers & Transitions Logic
  // --- Check Sudden Loud Acoustic Noise (Clap, Whistle, Shout) ---
  if (micAmplitude > MIC_SPIKE_PEAK_TO_PEAK) {
    if (currentState != STATE_SURPRISED) {
      changeState(STATE_SURPRISED);
    }
  }

  // --- Check High-Velocity Spinning / Tumbling (Dizzy Trigger) ---
  if (gyroMagnitude > DIZZY_GYRO_THRESHOLD) {
    if (dizzyStartTime == 0) {
      dizzyStartTime = now;
    } else if (now - dizzyStartTime > DIZZY_TRIGGER_TIME_MS) {
      changeState(STATE_DIZZY);
    }
  } else {
    dizzyStartTime = 0;
  }

  // --- Check Gentle Rhythmic Shaking (Happy Trigger) ---
  if (currentState != STATE_DIZZY && currentState != STATE_SLEEPING) {
    if (!shakeHighMet && accelMagnitude > SHAKE_ACCEL_HIGH) {
      shakeHighMet = true;
      lastShakeTime = now;
    } else if (shakeHighMet && accelMagnitude < SHAKE_ACCEL_LOW) {
      shakeHighMet = false;
      shakeCounter++;
      lastShakeTime = now;
      if (shakeCounter >= SHAKE_COUNT_TRIGGER) {
        shakeCounter = 0;
        changeState(STATE_HAPPY);
      }
    }

    if (now - lastShakeTime > SHAKE_WINDOW_MS) {
      shakeCounter = 0;
      shakeHighMet = false;
    }
  }

  // --- Check Flat On Desk (Sleep Trigger) ---
  // When lying flat: Z is close to 1.0g, X and Y are near zero, and device is stationary
  bool isFlat = (az >= FLAT_Z_MIN) &&
                (fabs(ax) < FLAT_XY_MAX) &&
                (fabs(ay) < FLAT_XY_MAX) &&
                (gyroMagnitude < FLAT_GYRO_MAX);

  if (isFlat) {
    if (currentState == STATE_IDLE && (now - lastFlatTime > SLEEP_ENTER_TIME_MS)) {
      changeState(STATE_SLEEPING);
    }
  } else {
    lastFlatTime = now; // Reset stillness timer whenever picked up or tilted
    if (currentState == STATE_SLEEPING) {
      // Woken up by being picked up
      changeState(STATE_SURPRISED);
    }
  }

  // 5. State-Specific Behaviors and Recovery Timers
  switch (currentState) {
    case STATE_IDLE:
      // Breathing bobbing animation (2 frames: alternating every 600ms)
      if (now > nextIdleBobTime) {
        nextIdleBobTime = now + 600;
        idleBobOffset = (idleBobOffset == 0) ? 2 : 0;
      }

      // Eye Blinking Animation
      if (!isBlinking && now >= nextBlinkTime) {
        isBlinking = true;
        blinkStartTime = now;
      }
      if (isBlinking && (now - blinkStartTime >= BLINK_DURATION_MS)) {
        isBlinking = false;
        nextBlinkTime = now + random(BLINK_INTERVAL_MIN_MS, BLINK_INTERVAL_MAX_MS);
      }

      // Random Glancing
      if (now >= nextGlanceTime) {
        int glanceChoice = random(0, 3);
        eyeGlanceX = (glanceChoice == 0) ? -3 : (glanceChoice == 1 ? 3 : 0);
        nextGlanceTime = now + random(2500, 5000);
      }
      break;

    case STATE_SLEEPING:
      // Snore sound at regular intervals
      if (now >= nextSnoreTime) {
        triggerSound(3);
        nextSnoreTime = now + SNORE_INTERVAL_MS;
      }

      // Spawn floating "Zzz" particles
      if (now >= nextParticleTime) {
        spawnParticle(0, (SCREEN_WIDTH / 2) + 28, (SCREEN_HEIGHT / 2) - 10);
        nextParticleTime = now + 1400;
      }
      break;

    case STATE_HAPPY:
      // Spawn floating Heart particles
      if (now >= nextParticleTime) {
        spawnParticle(1, (SCREEN_WIDTH / 2) + random(-30, 30), (SCREEN_HEIGHT / 2) - 5);
        nextParticleTime = now + 450;
      }

      // Auto-recover back to Idle after duration
      if (now - stateTimer > HAPPY_DURATION_MS) {
        resetParticles();
        changeState(STATE_IDLE);
      }
      break;

    case STATE_DIZZY:
      // Auto-recover back to Idle after duration
      if (now - stateTimer > DIZZY_DURATION_MS) {
        changeState(STATE_IDLE);
      }
      break;

    case STATE_SURPRISED:
      // Stay surprised briefly, then calm down to Idle
      if (now - stateTimer > SURPRISED_DURATION_MS) {
        changeState(STATE_IDLE);
      }
      break;
  }

  // 6. Particle Physics Update
  updateParticles();

  // 7. Audio Engine Tick (Non-blocking buzzer control)
  updateAudio(now);

  // 8. Display Frame Rendering (~30 FPS)
  if (now - lastFrameTime >= FRAME_INTERVAL_MS) {
    lastFrameTime = now;
    renderPet(now);
  }
}
