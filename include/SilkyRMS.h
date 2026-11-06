#ifndef SILKY_RMS_H
#define SILKY_RMS_H

#include <Arduino.h>
#include <stdexcept>

typedef uint32_t (*adc_read_cb_t)();

class SilkyRMS {
public:
    SilkyRMS(uint8_t adc_resolution_bits = 12, double v_ref = 3.3, double volts_to_amps = 30.0, double ac_voltage = 230.0);
    ~SilkyRMS();

    // Owns hardware resources (continuous ADC driver) - not copyable
    SilkyRMS(const SilkyRMS&) = delete;
    SilkyRMS& operator=(const SilkyRMS&) = delete;

    void calibrate_offset(uint32_t num_samples = 10000, uint32_t warmup_delay_ms = 1000);

    void run(uint32_t window_ms, double* out_power_w, double* out_irms);

    void begin_adc_native(uint8_t pin);
    void begin_adc_external(adc_read_cb_t callback);

    // Continuous (DMA) sampling on an ADC1 pin: gap-free, hardware-timed, and the
    // CPU sleeps while the DMA fills buffers. Requires ESP32 Arduino core 3.x
    // (ESP-IDF 5) and adc_resolution_bits == 12. Returns false if unsupported
    // (older core, non-ADC1 pin, bad frequency or driver error) - in that case
    // you can fall back to begin_adc_native().
    bool begin_adc_continuous(uint8_t pin, uint32_t sample_freq_hz = 40000);

    uint32_t do_read_adc();

    double get_dc_offset() const;
    uint32_t get_last_sample_count() const;

    void set_yield_params(uint32_t samples_per_yield, uint32_t yield_delay_ms);
    void set_ema_alpha(double alpha);
    void set_initial_dc_offset(double offset);

private:
    static const uint8_t NO_PIN = 0xFF;   // external ADC callback in use

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

    // Continuous (DMA) ADC state - opaque so the header doesn't need IDF 5 headers
    void* _cont_handle;
    uint8_t* _cont_buf;
    uint32_t _cont_buf_len;
    uint32_t _cont_buf_pos;
    uint8_t _cont_channel;
    bool _cont_running;

    void cont_restart();
    void cont_stop();
    bool cont_fill();
    bool cont_next(uint32_t* out_raw);
};

#endif // SILKY_RMS_H