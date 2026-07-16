# SilkyRMS

A high-performance, RTOS-aware AC signal RMS calculation library for ESP32 and modern dual-core microcontrollers. 

SilkyRMS calculates continuous True RMS current and power by utilizing high-speed hardware integer accumulation and an Exponential Moving Average (EMA) DC offset tracker. It is designed to maximize sample rates without starving RTOS background tasks or triggering watchdog panics.

## Core Mechanics

* **High-Speed Integer DSP Loop:** Bypasses software-emulated floating-point math during the active sampling window. On an ESP32-S3, this yields approximately 89,000 samples over a 5-second measurement window (~17.8 kHz sampling rate).
* **Dynamic DC Offset Tracking:** Resistor tolerances and thermal drift cause the actual resting voltage of a circuit to deviate from the theoretical midpoint (e.g., 2048 on a 12-bit ADC). SilkyRMS seeds the offset via a calibration routine and continuously adjusts it using an EMA filter, guaranteeing mathematical symmetry around the AC waveform.
* **Prime Number Aliasing Protection:** System yields (required for FreeRTOS IDLE tasks and WiFi) introduce sampling blind spots. SilkyRMS uses prime-number modulo boundaries (e.g., yielding for 2ms every 199 samples) to force these blind spots to precess mathematically across the 50Hz/60Hz AC phase. This prevents systematic undersampling of wave peaks.
* **Variable ADC Resolution:** Calculates dynamic boundaries based on the hardware bit depth provided.

## Compatible Hardware

SilkyRMS is built for non-invasive AC Current Transformers (CT clamps) that output a low AC voltage proportional to the current flowing through the live wire.

It works with clamps like the **SCT-013 series** (e.g., 30A/1V, 100A/1V). The library expects the AC signal from the clamp to be safely biased by a DC voltage divider network before entering the microcontroller's ADC pin.

## Hardware Wiring

The CT clamp outputs an AC waveform that swings positive and negative. Because the ESP32 ADC cannot read negative voltages, the signal must be elevated by a DC bias circuit (a voltage divider) to exactly half of the ADC reference voltage (1.65V). 

A bypass capacitor is required at the midpoint to stabilize the DC reference against noise and AC fluctuations.

       3.3V (ESP32)
        │
       [R1] 10kΩ
        │
        ├────────────────────── CT Clamp (Sleeve / Terminal 1)
        │
        ├──────────┐
        │          │
       [R2]       === C1 (10µF)
       10kΩ        │
        │          │
       GND        GND

    CT Clamp (Tip / Terminal 2) ───────── ESP32 ADC Pin (e.g., GPIO 4)

## Constructor Parameters

```cpp
SilkyRMS(uint8_t pin, 
         uint8_t adc_resolution_bits = 12, 
         double v_ref = 3.3, 
         double volts_to_amps = 30.0, 
         double ac_voltage = 230.0);
```

* `pin`: The GPIO pin connected to the CT clamp circuit.
* `adc_resolution_bits`: The bit depth of your MCU's ADC (e.g., 12 for most ESP32s).
* `v_ref`: The analog reference voltage powering your MCU (typically 3.3V).
* `volts_to_amps`: The ratio of your specific CT clamp. If your clamp produces 1V at 30A, this value is `30.0`. 
* `ac_voltage`: Your local grid mains voltage (e.g., `230.0` for EU, `120.0` for US). Used to calculate final wattage.

## Usage

Include the library, initialize the object with your hardware configuration, and pass memory addresses to the `run()` method.

```cpp
#include <Arduino.h>
#include <SilkyRMS.h>

// Initialize for a 30A/1V CT clamp on a 230V grid, using GPIO 4
SilkyRMS powerSensor(4, 12, 3.3, 30.0, 230.0);

void setup() {
    Serial.begin(115200);
    
    powerSensor.begin();
    
    // Measures 10,000 samples to establish the actual hardware resting point
    // Ensure 0A is flowing through the mains wire during this step
    powerSensor.calibrate_offset();
}

void loop() {
    double power_w = 0.0;
    double irms = 0.0;

    // Block for 5000ms. Library handles ADC reads, variance algebra, and RTOS yields.
    powerSensor.run(5000, &power_w, &irms);

    Serial.printf("Power: %.2f W | Current: %.2f A | Offset: %.2f | Samples: %lu\n", 
                  power_w, 
                  irms, 
                  powerSensor.get_dc_offset(),
                  powerSensor.get_last_sample_count());
}
```