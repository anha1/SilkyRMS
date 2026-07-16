#include "SilkyRMS.h"

SilkyRMS::SilkyRMS(uint8_t adc_resolution_bits, double v_ref, double volts_to_amps, double ac_voltage) {
    _resolution_bits = adc_resolution_bits;
    _v_ref = v_ref;
    _volts_to_amps = volts_to_amps;
    _ac_voltage = ac_voltage;
    
    // Dynamically calculate ADC Max (e.g., 12-bit -> 4095)
    _adc_max = (double)((1 << _resolution_bits) - 1);
    
    _dc_offset = (_adc_max + 1.0) / 2.0; 
    _ema_alpha = 0.03; 
    _samples_per_yield = 199; 
    _yield_delay_ms = 2;
    _last_sample_count = 0;
}

uint32_t SilkyRMS::do_read_adc() {
    return _read_adc ? _read_adc() : analogRead(_pin);
}

void SilkyRMS::calibrate_offset(uint32_t num_samples, uint32_t warmup_delay_ms) {
    if(!_is_begin_done) {
        throw std::invalid_argument("begin_adc_native/begin_adc_external - please setup one of these");
    }

    uint32_t measure_samples = 0.66 * (float)num_samples;
    uint32_t warmup_samples = num_samples - measure_samples;
    
    // 33% throwaway readings - give the circuit some time to stabilize
    delay(warmup_delay_ms / 2);
    for (uint32_t i = 0; i < warmup_samples; i++) {
        do_read_adc();
        if (i % 1000 == 0) delay(1);
    }
    delay(warmup_delay_ms / 2);

    uint32_t sum = 0;
    for (uint32_t i = 0; i < measure_samples; i++) {
        sum += do_read_adc();
        if (i % 1000 == 0) delay(1);
    }
    _dc_offset = (double)sum / (double)measure_samples;
}

// on-board ADC
void SilkyRMS::begin_adc_native(uint8_t pin) {
    if(_is_begin_done) {
        throw std::invalid_argument("Begin is already done");
    }
    _pin = pin;
    _read_adc = nullptr;
    analogReadResolution(_resolution_bits);
    pinMode(_pin, INPUT);
    _is_begin_done = true;
}

// custom code to read external EDC
void SilkyRMS::begin_adc_external(adc_read_cb_t callback) {
    if(_is_begin_done) {
        throw std::invalid_argument("Begin is already done");
    }
    _pin = -1;
    _read_adc = callback;
    _is_begin_done = true;
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
    if(!_is_begin_done) {
        throw std::invalid_argument("begin_adc_native/begin_adc_external - please setup one of these");
    }

    uint32_t start_time = millis();
    uint32_t samples = 0;
    
    uint64_t sum_sq_raw = 0;
    uint64_t sum_raw = 0;

    // High-speed integer math loop, read as much ADC values as possible
    while ((millis() - start_time) < window_ms) {
        for (uint32_t i = 0; i < _samples_per_yield; i++) {
            uint32_t raw = do_read_adc();
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
    double batch_mean = (double)sum_raw / (double)samples;
    _dc_offset += (batch_mean - _dc_offset) * _ema_alpha;

    // Final Math
    double sum_raw_d = (double)sum_raw;
    double sum_sq = (double)sum_sq_raw - ((sum_raw_d * sum_raw_d) / (double)samples);
    if (sum_sq < 0.0) sum_sq = 0.0; 

    double raw_rms = sqrt(sum_sq / (double)samples);
    double rms_volts = raw_rms * (_v_ref / _adc_max);
    double irms = rms_volts * _volts_to_amps;
    double power_w = irms * _ac_voltage;

    // Write values to the memory addresses provided by the caller
    if (out_irms) *out_irms = irms;
    if (out_power_w) *out_power_w = power_w;
}