# 🐾 Moody Desk Pet for M5StickC Plus 1.1

An interactive pixel-art virtual companion ("Tamagotchi") crafted specifically for the **M5StickC Plus 1.1** (ESP32-PICO + ST7789v2 LCD + MPU6886 IMU + SPM1423 PDM Mic + AXP192 PMIC).

---

## ✨ Features & Behavioral States

The pet uses a non-blocking state machine with real-time sensor processing and zero-flicker double-buffered sprite rendering (~30 FPS):

| State | Trigger | Animation & Effects |
| :--- | :--- | :--- |
| **`IDLE`** | Resting state | Bobbing breathing animation, random eye blinking, glancing left/right. |
| **`SLEEPING`** | Lay device flat on table ($Z \approx 1.0g$) for $>5$ seconds | Closed eyes, floating animated **"Zzz"** particles, screen dims to 20% to save power, periodic low snores. |
| **`HAPPY`** | Gentle rhythmic shaking ($0.65g \leftrightarrow 1.35g$) or pressing **Button A** | Curved happy eyes (`^ _ ^`), hop bounces, floating **hearts**, upbeat ascending two-tone chirp. |
| **`DIZZY`** | Sustained high spin/tumbling ($> 250^\circ/\text{s}$) for $>1.4$s | Crossed **"X X"** spiral eyes, squiggly mouth, recovers automatically after 3s. |
| **`SURPRISED`** | Sharp acoustic noise (clap, shout, whistle) or picking up from sleep | Wide open startled eyes (`O _ O`), **"!"** alert icon, high-pitched squeak warning. |

---

## 🎮 Hardware Controls

- **Button A (M5 Front Button):** "Pet" the companion / cycle expressions / wake up if sleeping.
- **Button B (Side Button):** Toggle audio mute on/off (indicated by `[AUDIO]` / `[MUTED]` in top-left).
- **Status Bar:** Displays audio state and a live battery gauge computed from AXP192 PMIC voltage.

---

## 🛠️ Requirements & Setup

### 1. Arduino IDE Setup
1. Download and install **[Arduino IDE 2.x](https://www.arduino.cc/en/software)**.
2. Open **Arduino IDE** $\rightarrow$ **File** $\rightarrow$ **Preferences** (or `Ctrl + ,`).
3. In **Additional boards manager URLs**, add:
   ```text
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Open the **Boards Manager** (left sidebar icon) and search for **`esp32`** by *Espressif Systems*. Install version **`2.0.x`** or higher.

### 2. Install Required Libraries
Open the **Library Manager** (`Ctrl + Shift + I`) and install:
- **`M5StickCPlus`** by *M5Stack* (version `0.1.0` or higher).

*(No external audio or graphics dependencies required—uses built-in ESP32 `driver/i2s.h` and `TFT_eSprite`)*.

---

## 🚀 How to Flash and Run

### 1. Open the Project
Open the Arduino IDE, then click **File $\rightarrow$ Open** and select:
```
MoodyDeskPet/MoodyDeskPet.ino
```

### 2. Configure Board Settings
Under the **Tools** menu, configure the following options:

- **Board:** `M5StickC-Plus` *(or `M5StickC` if using ESP32 core 2.0.x)*
- **Upload Speed:** `1500000` *(or `921600` / `115200` if connection is unstable)*
- **Flash Frequency:** `80MHz`
- **Partition Scheme:** `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`
- **Port:** Select your M5Stick's USB Serial port:
  - **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
  - **macOS:** `/dev/cu.usbserial-XXXXXXXX` or `wchusbserial`
  - **Windows:** `COM3`, `COM4`, etc.

> **Note on USB Drivers:**
> M5StickC Plus 1.1 uses either the **CH9102** or **CP2104** USB-to-UART chip. If your computer doesn't detect the board, install the corresponding driver from [M5Stack Software Download](https://docs.m5stack.com/en/download).

### 3. Compile & Upload
1. Connect your M5StickC Plus to your computer using a USB-C cable.
2. Click the **Upload** button ($\rightarrow$) in Arduino IDE.
3. Once flashing completes, the device will restart and the Moody Pet will boot immediately!

---

## ⚙️ Tuning Sensitivity Thresholds

All sensor parameters are clearly defined at the top of [`MoodyDeskPet.ino`](MoodyDeskPet.ino):

```cpp
// --- Sleep detection ---
#define SLEEP_ENTER_TIME_MS       5000    // Flat duration before falling asleep
#define FLAT_Z_MIN                0.85f   // Flat desk: Z ~ 1.0g

// --- Dizziness detection ---
#define DIZZY_GYRO_THRESHOLD      250.0f  // Gyro deg/sec threshold

// --- Shake detection ---
#define SHAKE_ACCEL_HIGH          1.35f   // Acceleration magnitude peak
#define SHAKE_ACCEL_LOW           0.65f   // Acceleration magnitude trough
#define SHAKE_COUNT_TRIGGER       4       // Number of shakes needed

// --- Microphone noise sensitivity ---
#define MIC_SPIKE_PEAK_TO_PEAK    11000   // Lower = more sensitive to claps
```

---

## ❓ Troubleshooting

1. **Upload fails or times out:**
   - Make sure no serial monitor is holding the COM port open.
   - Hold the power button (bottom left) for 6 seconds to turn off, then press it once to power on before retrying.
   - On Linux, ensure your user has dialout permissions: `sudo usermod -a -G dialout $USER`.
2. **Device doesn't sleep on flat desk:**
   - Ensure the screen is facing upwards on a steady flat desk. If tilted, adjust `FLAT_Z_MIN` from `0.85f` to `0.80f`.
3. **Microphone is too sensitive or not triggering:**
   - Adjust `MIC_SPIKE_PEAK_TO_PEAK`. Higher values (e.g. `14000`) prevent false triggers in loud rooms; lower values (e.g. `8000`) detect softer claps.
