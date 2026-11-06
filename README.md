![SCT-013](docs/sensor.png)

# Friendly Advice
If you are working on a power monitoring project, consider using the [PZEM-004T](https://www.google.com/search?q=pzem-004t) module instead, as it provides far more accurate readings.
This library is mostly useful if you are limited to a standalone SCT-013 (or similar) sensor.

# SilkyRMS

Power and current calculation for ESP32 using SCT-013-style sensors.

A high-performance, RTOS-aware AC signal RMS calculation library for ESP32 and modern dual-core microcontrollers. 

SilkyRMS continuously calculates True RMS current and apparent power using high-speed integer accumulation with exact per-window DC removal. It is designed to maximize the sample rate without starving RTOS background tasks or triggering watchdog panics.

[![PlatformIO Registry](https://badges.registry.platformio.org/packages/anha1/library/SilkyRMS.svg)](https://registry.platformio.org/libraries/anha1/SilkyRMS)

## Core Mechanics

* **High-Speed Integer DSP Loop:** The sampling loop uses only integer math (no software-emulated double-precision floating point), so the sample rate is limited only by the ADC read itself. With `analogRead()` on an ESP32-S3, this yields approximately 89,000 samples over a 5-second measurement window (~17.8 kHz sampling rate).
* **DC Offset Tracking:** Resistor tolerances and thermal drift cause the actual resting voltage of the circuit to deviate from the theoretical midpoint (e.g., 2048 on a 12-bit ADC). SilkyRMS seeds the offset with a calibration routine `calibrate_offset()` and tracks it with an EMA filter. Samples are centered around this offset before accumulation, keeping the integer sums small (safe for high-resolution external ADCs) and reducing floating-point cancellation. Because variance is shift-invariant, an inaccurate offset never affects the result.
* **RTOS-Friendly Yielding:** The loop periodically yields (by default 2ms every 199 samples) so FreeRTOS IDLE tasks and WiFi keep running and the watchdog stays quiet. The yield period (~13ms on an ESP32-S3) does not line up with the 20ms/16.7ms mains period, and FreeRTOS tick jitter randomizes it further, so the resulting sampling gaps drift across the AC phase instead of repeatedly skipping the same part of the wave. Tunable via `set_yield_params()`.
* **Variable ADC Resolution:** Using provided hardware bit depth.
* **ESP32 12-bit ADC Support:** see [BasicMeasurement (embedded ADC)](/examples/BasicMeasurement/BasicMeasurement.ino).
* **Continuous (DMA) ADC Mode:** hardware-timed, gap-free sampling with the CPU sleeping while DMA fills buffers. See [Continuous ADC mode](#continuous-dma-adc-mode).
* **External ADC Support:** see [External ADC example](/examples/ExternalAdc/ExternalAdc.ino).

## Compatible Hardware

SilkyRMS is built for non-invasive AC Current Transformers (CT clamps) that output a low AC voltage proportional to the current flowing through the live wire.

It works with clamps such as the **SCT-013 series** (e.g., 30A/1V, 100A/1V). The library expects the AC signal from the clamp to be safely biased by a DC voltage divider before it enters the microcontroller's ADC pin.

## Hardware Wiring

The CT clamp outputs an AC waveform that swings both positive and negative. Because the ESP32 ADC cannot read negative voltages, the signal must be raised by a DC bias circuit (a voltage divider) to exactly half the ADC reference voltage (1.65V).

A bypass capacitor is required at the midpoint to stabilize the DC reference against noise and AC fluctuations.

       3.3V (ESP32)
        │
       [R1] 2kΩ
        │
        ├────────────────────── CT Clamp (Sleeve / Terminal 1)
        │
        ├──────────┐
        │          │
       [R2]       === C1 (10µF)
       2kΩ         │
        │          │
       GND        GND

    CT Clamp (Tip / Terminal 2) ───────── ESP32 ADC Pin (e.g., GPIO 4)

> **Note:** Use an **ADC1** pin. On the ESP32-S3, GPIO 4 is ADC1 and works fine. On the classic ESP32, GPIO 4 is ADC2, which cannot be used while WiFi is active — pick an ADC1 pin such as GPIO 32–39 instead.

## Constructor Parameters

```cpp
SilkyRMS(
         uint8_t adc_resolution_bits = 12, 
         double v_ref = 3.3, 
         double volts_to_amps = 30.0, 
         double ac_voltage = 230.0);
```

* `adc_resolution_bits`: The bit depth of your MCU's ADC (e.g., 12 for most ESP32s).
* `v_ref`: The analog reference voltage powering your MCU (typically 3.3V).
* `volts_to_amps`: The ratio of your specific CT clamp. If your clamp outputs 1V at 30A, this value is `30.0`.
* `ac_voltage`: Your local grid mains voltage (e.g., `230.0` for EU, `120.0` for US). Used to calculate the final wattage.

> **Note:** The reported power is **apparent power** (`Irms × ac_voltage`, in VA), assuming a fixed mains voltage and a power factor of 1. The library measures current only, so it cannot see actual line voltage or phase shift. Loads with a poor power factor (switching power supplies, motors, standby electronics) will read higher than on a real-power meter such as the PZEM.

## Usage Examples

See:
* [BasicMeasurement (embedded ADC)](/examples/BasicMeasurement/BasicMeasurement.ino) 
* [External ADC](/examples/ExternalAdc/ExternalAdc.ino).
* [Continuous (DMA) ADC](/examples/ContinuousAdc/ContinuousAdc.ino).

## Continuous (DMA) ADC Mode

By default, SilkyRMS polls `analogRead()` in a tight loop. That limits the sample rate to how fast `analogRead()` is (~17.8 kHz on an ESP32-S3), keeps the CPU busy, and needs periodic yields that leave small gaps in the data.

On **ESP32 Arduino core 3.x** (ESP-IDF 5) you can instead use the ADC's continuous mode, where the hardware samples at a fixed rate and DMA delivers buffers:

```cpp
if (!powerSensor.begin_adc_continuous(4, 40000)) { // ADC1 pin, sample rate in Hz
    powerSensor.begin_adc_native(4);              // fallback
}
```

* **Gap-free and hardware-timed:** every sample is evenly spaced, with no yield gaps.
* **CPU-friendly:** the measuring task sleeps while DMA fills buffers, so WiFi and other tasks get more time. The yield settings (`set_yield_params()`) are not used in this mode.
* **Same API:** `calibrate_offset()`, `run()` and everything else work unchanged.

Requirements and limitations:
* ESP32 Arduino core **3.x** only. On older cores (including the default PlatformIO `espressif32` platform, which ships core 2.x), `begin_adc_continuous()` returns `false` — use [pioarduino](https://github.com/pioarduino/platform-espressif32) for core 3.x.
* **ADC1 pins only**, and `adc_resolution_bits` must be `12`.
* The supported sample rate depends on the chip (e.g., 611 Hz – 83.3 kHz on ESP32-S3, 20 kHz – 2 MHz on the classic ESP32). Out-of-range values make `begin_adc_continuous()` return `false`.
* Continuous mode owns ADC1 — don't call `analogRead()` on other ADC1 pins in the same sketch.
* **One instance per sketch:** only one `SilkyRMS` object can use continuous mode at a time, since there is a single ADC1 DMA driver. To monitor several clamps, use `begin_adc_native()` for each of them instead.
* The DMA is started by `calibrate_offset()` and `run()` and stopped when they finish, so it is idle between measurements. Calling `do_read_adc()` directly also starts it, and it then keeps running until the next `calibrate_offset()` or `run()` completes.


## SilkyRMS vs emonlib readings (defaults)
![SilkyRMS vs emonlib](docs/compare.png)


## Calibration
The SilkyRMS vs emonlib chart above shows raw library readings. Using a PZEM module as a reference, it is clear that both libraries report a phantom
baseline power of about 100W. For my setup, I arrived at the following calibration formula applied on top of the SilkyRMS readings:

```cpp
float power_w_calibrated = max(0.0, 1.02 * power_w - 100.);
```

The clamp to zero matters: with no load connected, the linear correction can otherwise go slightly negative.

Interestingly, a **linear** offset fits much better than the textbook approach for random noise. Uncorrelated noise adds in quadrature, so it would normally be removed with `I = sqrt(I_meas² − I_noise²)` — but that only subtracts a few watts at high loads, and against the PZEM it leaves readings ~90W too high at 700W. A constant offset that matches across the whole range (and survives the exact per-window DC removal) points to a systematic error in the measurement chain, most likely ESP32 ADC nonlinearity (especially with 11dB attenuation), rather than random noise. Power supply quality also affects it.

With this correction applied, PZEM and SilkyRMS readings are nearly identical in practice:
![SilkyRMS calibrated vs PZEM](docs/calibration.png)

Note that this formula works for my environment (verified from ~90W to ~750W; below that it is untested), but I'm not sure it's a generic adjustment — the same goes for the ~100W baseline. Remaining small deviations, such as SilkyRMS reading somewhat higher in a few periods, are consistent with the apparent-vs-real power difference described above. That's why this correction is not built into the library. Treat my results as a reference point and choose the calibration that fits your own hardware.