#include "stress_detector.h"

// Weighting based on Kessler & Turner (1997) Cat Stress Score (CSS)
// CSS 1-2 (Relaxed/Neutral) -> 0.00
// CSS 3-4 (Focus/Vigilance) -> 0.20
// CSS 5-6 (Scared/Fear)     -> 0.85
// CSS 6-7 (Angry/Distress)  -> 1.00
static const float WEIGHT_RELAX  = 0.00f;
static const float WEIGHT_FOCUS  = 0.20f;
static const float WEIGHT_SCARED = 0.85f;
static const float WEIGHT_ANGRY  = 1.00f;

// Algorithm parameters
static const float  STRESS_ALERT_INDEX_THRESHOLD = 0.65f; // Minimum smoothed index for alert
static const uint32_t DISTRESS_TIME_THRESHOLD_MS = 8000;  // 8 seconds sustained duration
static const uint32_t ALERT_COOLDOWN_MS          = 300000; // 5 minutes cooldown between alerts

static float    s_smoothed_stress = 0.0f;
static uint32_t s_last_sample_time = 0;
static uint32_t s_consecutive_distress_ms = 0;
static uint32_t s_last_alert_time = 0;

static int   s_last_dominant_emotion = EMOTION_RELAX;
static float s_accum_conf = 0.0f;
static int   s_distress_sample_count = 0;

void initStressDetector() {
    s_smoothed_stress = 0.0f;
    s_last_sample_time = millis();
    s_consecutive_distress_ms = 0;
    s_last_alert_time = 0;
    s_last_dominant_emotion = EMOTION_RELAX;
    s_accum_conf = 0.0f;
    s_distress_sample_count = 0;
    Serial.println("[Stress Detector] Initialized TFSI engine based on Cat Stress Score (CSS)");
}

void updateStressSample(int emotion_class, float confidence) {
    uint32_t now = millis();
    uint32_t elapsed_ms = (s_last_sample_time > 0 && now >= s_last_sample_time) ? (now - s_last_sample_time) : 100;
    s_last_sample_time = now;

    if (elapsed_ms > 2000) {
        elapsed_ms = 100; // Reset after long pauses
    }

    float weight = 0.0f;
    switch (emotion_class) {
        case EMOTION_RELAX:  weight = WEIGHT_RELAX; break;
        case EMOTION_FOCUS:  weight = WEIGHT_FOCUS; break;
        case EMOTION_SCARED: weight = WEIGHT_SCARED; break;
        case EMOTION_ANGRY:  weight = WEIGHT_ANGRY; break;
        default:             weight = 0.0f; break;
    }

    float sample_score = weight * confidence;

    // EWMA Smoothing with time constant tau = 4.0 seconds (4000 ms)
    float dt = (float)elapsed_ms / 1000.0f;
    float alpha = dt / (4.0f + dt);
    s_smoothed_stress = (1.0f - alpha) * s_smoothed_stress + alpha * sample_score;

    // Leaky duration integrator for acute distress (scared or angry)
    if (sample_score >= 0.55f && (emotion_class == EMOTION_SCARED || emotion_class == EMOTION_ANGRY)) {
        s_consecutive_distress_ms += elapsed_ms;
        s_last_dominant_emotion = emotion_class;
        s_accum_conf += confidence;
        s_distress_sample_count++;
    } else if (sample_score < 0.25f) {
        // Decay faster when cat is relaxed
        uint32_t decay = elapsed_ms * 2;
        if (s_consecutive_distress_ms > decay) {
            s_consecutive_distress_ms -= decay;
        } else {
            s_consecutive_distress_ms = 0;
            s_accum_conf = 0.0f;
            s_distress_sample_count = 0;
        }
    }
}

bool evaluateStressAnomaly(StressReport* report) {
    uint32_t now = millis();
    bool trigger = false;

    if (s_smoothed_stress >= STRESS_ALERT_INDEX_THRESHOLD &&
        s_consecutive_distress_ms >= DISTRESS_TIME_THRESHOLD_MS) {
        // Check cooldown
        if (s_last_alert_time == 0 || (now - s_last_alert_time >= ALERT_COOLDOWN_MS)) {
            trigger = true;
            s_last_alert_time = now;
        }
    }

    if (report != nullptr) {
        report->stress_index = s_smoothed_stress;
        report->consecutive_distress_sec = (float)s_consecutive_distress_ms / 1000.0f;
        report->dominant_emotion = s_last_dominant_emotion;
        report->avg_confidence = (s_distress_sample_count > 0) ? (s_accum_conf / (float)s_distress_sample_count) : 0.8f;
        report->is_alert_triggered = trigger;
    }

    return trigger;
}

float getCurrentStressScore() {
    return s_smoothed_stress;
}

const char* getEmotionName(int emotion_class) {
    switch (emotion_class) {
        case EMOTION_ANGRY:  return "Angry";
        case EMOTION_FOCUS:  return "Focus";
        case EMOTION_RELAX:  return "Relax";
        case EMOTION_SCARED: return "Scared";
        default:             return "Unknown";
    }
}
