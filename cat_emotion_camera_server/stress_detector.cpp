#include "stress_detector.h"

// ============================================================================
// Feline Stress Detection Algorithm
// Grounded in peer-reviewed animal behavior & affective computing literature:
// 1. Kessler, M. R., & Turner, D. C. (1997). Stress and adaptation of cats
//    (Felis silvestris catus) housed singly, in pairs and in groups in boarding
//    catteries. Animal Welfare, 6(3), 243-254.
// 2. Evangelista, M. C., et al. (2019). Facial expressions of pain in cats:
//    the development and validation of Feline Grimace Scale. Nature Scientific
//    Reports, 9, 19128.
// 3. Caeiro, C. C., Burrows, A. M., & Waller, B. M. (2017). Development and
//    application of CatFACS. Applied Animal Behaviour Science, 189, 66-78.
// 4. Stella, J., Croney, C., & Buffington, T. (2013). Effects of stressors on
//    the behavior and physiology of domestic cats. J Feline Med Surg, 15(7), 578-586.
// 5. Yeon, S. C., et al. (2002). Differences in vocalization between feral and
//    domestic cats. Applied Animal Behaviour Science, 77(3), 209-221.
// ============================================================================

// Weighting parameters based on Cat Stress Score (CSS 1-7)
// CSS 1-2 (Very Relaxed / Relaxed)        -> 0.00
// CSS 3-4 (Weakly Tense / Attentive Focus)-> 0.15
// CSS 5-6 (Anxious / Fearful / Scared)    -> 0.85
// CSS 6-7 (Terrified / Defensive Agonistic)-> 1.00
static const float WEIGHT_RELAX  = 0.00f;
static const float WEIGHT_FOCUS  = 0.15f;
static const float WEIGHT_SCARED = 0.85f;
static const float WEIGHT_ANGRY  = 1.00f;

// Dual-threshold hysteresis and timing parameters
static const float    STRESS_ALERT_INDEX_THRESHOLD = 0.65f; // Trigger threshold (CSS 5-6 equivalent)
static const float    STRESS_RESET_INDEX_THRESHOLD = 0.35f; // De-escalation reset threshold
static const uint32_t DISTRESS_TIME_THRESHOLD_MS   = 3000;  // 3 seconds sustained duration
static const uint32_t ALERT_COOLDOWN_MS            = 45000; // 45 seconds cooldown between Discord alerts

static float    s_smoothed_stress = 0.0f;
static float    s_latest_raw_score = 0.0f;
static uint32_t s_last_sample_time = 0;
static uint32_t s_consecutive_distress_ms = 0;
static uint32_t s_last_alert_time = 0;
static bool     s_in_alert_state = false;

static int   s_last_dominant_emotion = EMOTION_RELAX;
static float s_accum_conf = 0.0f;
static int   s_distress_sample_count = 0;

void initStressDetector() {
    s_smoothed_stress = 0.0f;
    s_latest_raw_score = 0.0f;
    s_last_sample_time = millis();
    s_consecutive_distress_ms = 0;
    s_last_alert_time = 0;
    s_in_alert_state = false;
    s_last_dominant_emotion = EMOTION_RELAX;
    s_accum_conf = 0.0f;
    s_distress_sample_count = 0;
    Serial.println("[Stress Detector] Initialized TFSI engine with Kessler & Turner CSS + FGS models");
}

int calculateCSSLevel(float score) {
    if (score < 0.15f) return 1; // Very Relaxed
    if (score < 0.30f) return 2; // Relaxed
    if (score < 0.45f) return 3; // Weakly Tense
    if (score < 0.60f) return 4; // Very Tense
    if (score < 0.75f) return 5; // Anxious
    if (score < 0.90f) return 6; // Distressed / Scared
    return 7;                    // Terrified / High Agonistic
}

int getEstimatedCSSLevel() {
    return calculateCSSLevel(s_smoothed_stress);
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
        default:             weight = 0.00f; break;
    }

    float sample_score = weight * confidence;
    s_latest_raw_score = sample_score;

    // EWMA Smoothing with time constant tau = 3.5 seconds
    float dt = (float)elapsed_ms / 1000.0f;
    float alpha = dt / (3.5f + dt);
    s_smoothed_stress = (1.0f - alpha) * s_smoothed_stress + alpha * sample_score;

    // Acute distress accumulator with confidence-weighted surge acceleration (Stella et al. 2013)
    if (sample_score >= 0.50f && (emotion_class == EMOTION_SCARED || emotion_class == EMOTION_ANGRY)) {
        float surge_multiplier = 1.0f;
        if (confidence > 0.80f) {
            // Rapid sympathetic escalation for high-certainty distress cues
            surge_multiplier += (confidence - 0.80f) * 2.0f; // 1.0 to 1.4x acceleration
        }
        uint32_t increment_ms = (uint32_t)((float)elapsed_ms * surge_multiplier);
        s_consecutive_distress_ms += increment_ms;
        s_last_dominant_emotion = emotion_class;
        s_accum_conf += confidence;
        s_distress_sample_count++;
    } else if (sample_score < 0.25f) {
        // Graceful decay when cat returns to relaxed or neutral state (tolerates momentary frame misses)
        uint32_t decay = (elapsed_ms > 1) ? (elapsed_ms / 2) : 1;
        if (s_consecutive_distress_ms > decay) {
            s_consecutive_distress_ms -= decay;
        } else {
            s_consecutive_distress_ms = 0;
            s_accum_conf = 0.0f;
            s_distress_sample_count = 0;
        }
    }

    // Reset hysteresis alert state when stress score drops below the reset threshold
    if (s_in_alert_state && s_smoothed_stress <= STRESS_RESET_INDEX_THRESHOLD) {
        s_in_alert_state = false;
        Serial.println("[Stress Detector] Cat returned to baseline calm state (Hysteresis reset).");
    }
}

bool evaluateStressAnomaly(StressReport* report) {
    uint32_t now = millis();
    bool trigger = false;

    if (s_smoothed_stress >= STRESS_ALERT_INDEX_THRESHOLD &&
        s_consecutive_distress_ms >= DISTRESS_TIME_THRESHOLD_MS) {
        // Check cooldown period
        if (s_last_alert_time == 0 || (now - s_last_alert_time >= ALERT_COOLDOWN_MS)) {
            trigger = true;
            s_last_alert_time = now;
            s_in_alert_state = true;
        }
    }

    if (report != nullptr) {
        report->stress_index = s_smoothed_stress;
        report->raw_sample_score = s_latest_raw_score;
        report->consecutive_distress_sec = (float)s_consecutive_distress_ms / 1000.0f;
        report->dominant_emotion = s_last_dominant_emotion;
        report->avg_confidence = (s_distress_sample_count > 0) ? (s_accum_conf / (float)s_distress_sample_count) : 0.85f;
        report->css_level = calculateCSSLevel(s_smoothed_stress);
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

const char* getCSSLevelDescription(int css_level) {
    switch (css_level) {
        case 1: return "CSS Level 1: Very Relaxed";
        case 2: return "CSS Level 2: Relaxed";
        case 3: return "CSS Level 3: Weakly Tense";
        case 4: return "CSS Level 4: Very Tense";
        case 5: return "CSS Level 5: Anxious (Elevated Vigilance)";
        case 6: return "CSS Level 6: Distressed / Scared (Intervention Required)";
        case 7: return "CSS Level 7: Terrified / Agonistic Defense";
        default: return "CSS Level: Undetermined";
    }
}

float getConsecutiveDistressSec() {
    return (float)s_consecutive_distress_ms / 1000.0f;
}

uint32_t getAlertCooldownRemainingSec() {
    if (s_last_alert_time == 0) return 0;
    uint32_t now = millis();
    if (now - s_last_alert_time >= ALERT_COOLDOWN_MS) return 0;
    return (ALERT_COOLDOWN_MS - (now - s_last_alert_time)) / 1000;
}

void resetAlertCooldown() {
    s_last_alert_time = 0;
    s_in_alert_state = false;
}
