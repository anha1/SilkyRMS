#ifndef SILKY_RMS_H
#define SILKY_RMS_H

#include <Arduino.h>
#include <stdexcept>

typedef uint32_t (*adc_read_cb_t)();

class SilkyRMS {
public:
    SilkyRMS(uint8_t adc_resolution_bits = 12, double v_ref = 3.3, double volts_to_amps = 30.0, double ac_voltage = 230.0);

    void calibrate_offset(uint32_t num_samples = 10000, uint32_t warmup_delay_ms = 1000);

    void run(uint32_t window_ms, double* out_power_w, double* out_irms);

    void begin_adc_native(uint8_t pin);
    void begin_adc_external(adc_read_cb_t callback);

    uint32_t do_read_adc();

    double get_dc_offset() const;
    uint32_t get_last_sample_count() const;

    void set_yield_params(uint32_t samples_per_yield, uint32_t yield_delay_ms);
    void set_ema_alpha(double alpha);
    void set_initial_dc_offset(double offset);

private:
    uint8_t _pin;
    uint8_t _resolution_bits;
    adc_read_cb_t _read_adc;
    bool _is_begin_done;
    
    // Physics constants
    double _v_ref;
    double _adc_max;
    double _volts_to_amps;
    double _ac_voltage;

    // State accumulators
    double _dc_offset;
    double _ema_alpha;
    
    uint32_t _samples_per_yield;
    uint32_t _yield_delay_ms;
    uint32_t _last_sample_count;

};

#endif // SILKY_RMS_H