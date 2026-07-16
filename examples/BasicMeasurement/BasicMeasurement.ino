#include <Arduino.h>
#include <SilkyRMS.h>

// Initialize the library with your hardware profile:
// SilkyRMS(PIN, ADC_BITS, V_REF, VOLTS_TO_AMPS, AC_VOLTAGE)
SilkyRMS powerSensor(4, 12, 3.3, 30.0, 230.0);

void setup() {
    Serial.begin(115200);

    powerSensor.begin();
    
    Serial.println("Calibrating hardware DC offset... please wait.");
    powerSensor.calibrate_offset(); 
    
    Serial.printf("Calibration complete. Midpoint: %.2f\n", powerSensor.get_dc_offset());
    Serial.println("Starting continuous DSP measurement...");
}

void loop() {
    double power_w = 0.0;
    double irms = 0.0;

    // Block and measure for 5 seconds. The library populates the variables via pointers.
    powerSensor.run(5000, &power_w, &irms);

    // Print the telemetry to the serial monitor
    Serial.printf("Power: %8.2f W | Current: %5.2f A | Offset: %7.2f | Samples: %lu\n", 
                  power_w, 
                  irms, 
                  powerSensor.get_dc_offset(),
                  powerSensor.get_last_sample_count());
}