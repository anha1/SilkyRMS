#include "SilkyRMS.h"

SilkyRMS::SilkyRMS(uint8_t pin, uint8_t adc_resolution_bits, double v_ref, double volts_to_amps, double ac_voltage) {
    _pin = pin;
    _resolution_bits = adc_resolution_bits;
    _v_ref = v_ref;
    _volts_to_amps = volts_to_amps;
    _ac_voltage = ac_voltage;
    
    // Dynamically calculate ADC Max (e.g., 12-bit -> 4095)
    _adc_max = (double)((1 << _resolution_bits) - 1);
    
    _dc_offset = (_adc_max + 1.0) / 2.0; 
    _ema_alpha = 0.05; 
    _samples_per_yield = 199; 
    _yield_delay_ms = 2;
    _last_sample_count = 0;
}

void SilkyRMS::begin() {
    analogReadResolution(_resolution_bits);
    pinMode(_pin, INPUT);
}

void SilkyRMS::calibrate_offset(uint32_t num_samples) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < num_samples; i++) {
        sum += analogRead(_pin);
        if (i % 1000 == 0) delay(1);
    }
    _dc_offset = (double)sum / num_samples;
}

void SilkyRMS::set_yield_params(uint32_t samples_per_yield, uint32_t yield_delay_ms) {
    _samples_per_yield = samples_per_yield;
    _yield_delay_ms = yield_delay_ms;
}

void SilkyRMS::set_ema_alpha(double alpha) { _ema_alpha = alpha; }
void SilkyRMS::set_initial_dc_offset(double offset) { _dc_offset = offset; }
double SilkyRMS::get_dc_offset() const { return _dc_offset; }
uint32_t SilkyRMS::get_last_sample_count() const { return _last_sample_count; }

void SilkyRMS::run(uint32_t window_ms, double* out_power_w, double* out_irms) {
    uint32_t start_time = millis();
    uint32_t samples = 0;
    
    uint64_t sum_sq_raw = 0;
    uint32_t sum_raw = 0;

    // High-speed integer math loop
    while ((millis() - start_time) < window_ms) {
        for (uint32_t i = 0; i < _samples_per_yield; i++) {
            uint32_t raw = analogRead(_pin);
            sum_raw += raw;
            sum_sq_raw += (raw * raw);
        }
        samples += _samples_per_yield;
        delay(_yield_delay_ms);
    }

    _last_sample_count = samples;

    // Safety catch for zero samples
    if (samples == 0) {
        if (out_power_w) *out_power_w = 0.0;
        if (out_irms) *out_irms = 0.0;
        return;
    }

    // DC Offset EMA Tracker
    double batch_mean = (double)sum_raw / samples;
    _dc_offset += (batch_mean - _dc_offset) * _ema_alpha;

    // Variance Algebra 
    double sum_raw_d = (double)sum_raw;
    double sum_sq = (double)sum_sq_raw - ((sum_raw_d * sum_raw_d) / samples);
    if (sum_sq < 0.0) sum_sq = 0.0; 

    // Final Math
    double raw_rms = sqrt(sum_sq / samples);
    double rms_volts = raw_rms * (_v_ref / _adc_max);
    double irms = rms_volts * _volts_to_amps;
    double power_w = irms * _ac_voltage;

    // Write values to the memory addresses provided by the caller
    if (out_irms) *out_irms = irms;
    if (out_power_w) *out_power_w = power_w;
}