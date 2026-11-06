#include <Arduino.h>
#include <SilkyRMS.h>

// Continuous (DMA) mode: hardware-timed, gap-free sampling.
// Requires ESP32 Arduino core 3.x and an ADC1 pin (GPIO 4 is ADC1 on ESP32-S3;
// use GPIO 32-39 on the classic ESP32).
SilkyRMS powerSensor(12, 3.3, 30.0, 230.0);

void setup() {
    Serial.begin(115200);

    if (!powerSensor.begin_adc_continuous(4, 40000)) { // ADC PIN, sample rate in Hz
        Serial.println("Continuous ADC not available, falling back to analogRead()");
        powerSensor.begin_adc_native(4);
    }

    Serial.println("Calibrating hardware DC offset... please wait.");
    powerSensor.calibrate_offset();

    Serial.printf("Calibration complete. Midpoint: %.2f\n", powerSensor.get_dc_offset());
}

void loop() {
    double power_w = 0.0;
    double irms = 0.0;

    powerSensor.run(5000, &power_w, &irms);

    Serial.printf("Power: %8.2f W | Current: %5.2f A | Offset: %7.2f | Samples: %lu\n",
                  power_w,
                  irms,
                  powerSensor.get_dc_offset(),
                  (unsigned long)powerSensor.get_last_sample_count());
}
