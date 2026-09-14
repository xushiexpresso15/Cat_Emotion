#include "stress_detector.h"

// ============================================================================
// Feline Stress Detection Algorithm (Bout-Based Ethological Engine)
// Grounded in peer-reviewed animal behavior & affective computing literature:
// 1. Kessler, M. R., & Turner, D. C. (1997). Stress and adaptation of cats
//    (Felis silvestris catus) housed singly, in pairs and in groups in boarding
//    catteries. Animal Welfare, 6(3), 243-254.
// 2. Stella, J., Croney, C., & Buffington, T. (2013). Effects of stressors on
//    the behavior and physiology of domestic cats. J Feline Med Surg, 15(7), 578-586.
// 3. Evangelista, M. C., et al. (2019). Facial expressions of pain in cats:
//    the development and validation of Feline Grimace Scale. Nature Scientific Reports, 9, 19128.
// 4. Martin, P., & Bateson, P. (2007). Measuring Behaviour: An Introductory Guide.
//    Cambridge University Press. (Behavioral Bout Duration & Bout Criterion Interval).
// ============================================================================

// Weighting parameters based on Cat Stress Score (CSS 1-7)
static const float WEIGHT_RELAX  = 0.00f; // CSS 1-2
static const float WEIGHT_FOCUS  = 0.15f; // CSS 3-4
static const float WEIGHT_SCARED = 0.85f; // CSS 5-6
static const float WEIGHT_ANGRY  = 1.00f; // CSS 6-7

// Ethological Timing Parameters:
// 1. DISTRESS_TIME_THRESHOLD_MS: 6.0 seconds sustained duration.
//    (Stella et al. 2013: Filters transient startle reflexes <2s, requires sustained distress >=5-6s)
// 2. BOUT_GAP_TOLERANCE_MS: 2.0 seconds Bout Criterion Interval (BCI).
//    (Martin & Bateson 2007: Momentary sensor dropout or frame flicker <=2.0s does not break an ongoing bout,
//     but calm / non-distress persisting >=2.0s officially terminates the bout and resets the duration).
// 3. ALERT_COOLDOWN_MS: 45.0 seconds refractory period between Discord webhook alerts.
static const uint32_t DISTRESS_TIME_THRESHOLD_MS = 6000;
static const uint32_t BOUT_GAP_TOLERANCE_MS      = 2000;
static const uint32_t ALERT_COOLDOWN_MS          = 45000;

// Instantaneous confidence threshold for high-distress classification
// Raised to 0.55 to avoid false bouts triggered by ambiguous / low-confidence detections
static const float DISTRESS_CONFIDENCE_THRESHOLD = 0.55f;

// Continuous state tracking
static float    s_smoothed_stress = 0.0f;
static float    s_latest_raw_score = 0.0f;
static uint32_t s_last_sample_time = 0;

// Bout timing variables (Wall-clock real time)
static bool     s_bout_active = false;
static uint32_t s_bout_start_time_ms = 0;
static uint32_t s_last_distress_frame_ms = 0;
static bool     s_alert_sent_for_current_bout = false;

// Alert & Cooldown
static uint32_t s_last_alert_time = 0;

// Ethological voting counters for the active bout (prevents irreversible latching to Angry)
static uint32_t s_bout_total_samples = 0;
static uint32_t s_distress_sample_count = 0;
static uint32_t s_angry_count = 0;
static uint32_t s_scared_count = 0;
static uint32_t s_calm_count = 0;
static float    s_angry_conf_sum = 0.0f;
static float    s_scared_conf_sum = 0.0f;

static void resetBoutCounters() {
    s_bout_total_samples = 0;
    s_distress_sample_count = 0;
    s_angry_count = 0;
    s_scared_count = 0;
    s_calm_count = 0;
    s_angry_conf_sum = 0.0f;
    s_scared_conf_sum = 0.0f;
}

void initStressDetector() {
    s_smoothed_stress = 0.0f;
    s_latest_raw_score = 0.0f;
    s_last_sample_time = millis();
    s_bout_active = false;
    s_bout_start_time_ms = 0;
    s_last_distress_frame_ms = 0;
    s_alert_sent_for_current_bout = false;
    s_last_alert_time = 0;
    resetBoutCounters();
    Serial.println("[Stress Detector] Initialized Robust Multi-Cat Ethological Engine");
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

// Statistical majority voting: accurately determines true dominant distress state
static int calculateDominantDistressEmotion() {
    if (s_angry_count > s_scared_count) {
        return EMOTION_ANGRY;
    } else if (s_scared_count > s_angry_count) {
        return EMOTION_SCARED;
    } else if (s_angry_count > 0 && s_scared_count > 0) {
        float avg_angry = s_angry_conf_sum / (float)s_angry_count;
        float avg_scared = s_scared_conf_sum / (float)s_scared_count;
        return (avg_angry >= avg_scared) ? EMOTION_ANGRY : EMOTION_SCARED;
    } else if (s_angry_count > 0) {
        return EMOTION_ANGRY;
    } else if (s_scared_count > 0) {
        return EMOTION_SCARED;
    }
    return EMOTION_RELAX;
}

int getEstimatedCSSLevel() {
    if (s_bout_active) {
        int dom = calculateDominantDistressEmotion();
        if (dom == EMOTION_ANGRY) return 6;
        if (dom == EMOTION_SCARED) return 5;
    }
    return calculateCSSLevel(s_smoothed_stress);
}

void updateStressSample(int emotion_class, float confidence) {
    uint32_t now = millis();
    uint32_t elapsed_ms = (s_last_sample_time > 0 && now >= s_last_sample_time) ? (now - s_last_sample_time) : 100;
    s_last_sample_time = now;

    if (elapsed_ms > 2000) {
        elapsed_ms = 100;
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

    // Responsive EWMA for real-time dashboard display (tau = 1.2s)
    float dt = (float)elapsed_ms / 1000.0f;
    float alpha = dt / (1.2f + dt);
    s_smoothed_stress = (1.0f - alpha) * s_smoothed_stress + alpha * sample_score;

    // Check if current frame represents acute distress
    bool is_distress = (emotion_class == EMOTION_ANGRY || emotion_class == EMOTION_SCARED) &&
                       (confidence >= DISTRESS_CONFIDENCE_THRESHOLD);

    if (is_distress) {
        s_last_distress_frame_ms = now;
        if (!s_bout_active) {
            // New bout onset
            s_bout_active = true;
            s_bout_start_time_ms = now;
            s_alert_sent_for_current_bout = false;
            resetBoutCounters();
            Serial.printf("[Stress Detector] Distress bout onset (First emotion: %s, Conf: %.0f%%)\n",
                getEmotionName(emotion_class), confidence * 100.0f);
        }
        
        s_bout_total_samples++;
        s_distress_sample_count++;
        if (emotion_class == EMOTION_ANGRY) {
            s_angry_count++;
            s_angry_conf_sum += confidence;
        } else if (emotion_class == EMOTION_SCARED) {
            s_scared_count++;
            s_scared_conf_sum += confidence;
        }
    } else {
        if (s_bout_active) {
            s_bout_total_samples++;
            s_calm_count++;

            // Evaluate if bout has timed out via 2.0s gap tolerance OR has been overwhelmed by calm frames
            bool gap_exceeded = (now - s_last_distress_frame_ms >= BOUT_GAP_TOLERANCE_MS);
            bool calm_dominated = (s_calm_count >= 8 && ((float)s_distress_sample_count / (float)s_bout_total_samples < 0.35f));

            if (gap_exceeded || calm_dominated) {
                uint32_t total_bout_sec = (s_last_distress_frame_ms > s_bout_start_time_ms) ?
                    ((s_last_distress_frame_ms - s_bout_start_time_ms) / 1000) : 0;
                Serial.printf("[Stress Detector] Distress bout terminated after %lu s (Cat calm / distress density low). Resetting.\n",
                    (unsigned long)total_bout_sec);
                s_bout_active = false;
                s_bout_start_time_ms = 0;
                s_alert_sent_for_current_bout = false;
                resetBoutCounters();
            }
        }
    }
}

bool evaluateStressAnomaly(StressReport* report) {
    uint32_t now = millis();
    bool trigger = false;

    // Timeout check
    if (s_bout_active && (now - s_last_distress_frame_ms >= BOUT_GAP_TOLERANCE_MS)) {
        s_bout_active = false;
        s_bout_start_time_ms = 0;
        s_alert_sent_for_current_bout = false;
        resetBoutCounters();
    }

    float current_duration_sec = 0.0f;
    int dominant_emotion = calculateDominantDistressEmotion();
    float avg_conf = 0.85f;

    if (s_bout_active) {
        uint32_t duration_ms = (now >= s_bout_start_time_ms) ? (now - s_bout_start_time_ms) : 0;
        current_duration_sec = (float)duration_ms / 1000.0f;

        float distress_density = (s_bout_total_samples > 0) ? 
            ((float)s_distress_sample_count / (float)s_bout_total_samples) : 0.0f;

        // Trigger condition (Stella et al. 2013; Kessler & Turner 1997):
        // 1. Bout duration >= 6.0 seconds
        // 2. Distress density >= 50% (at least half the observed frames are acute distress)
        // 3. Distress sample count >= 10 frames (filters transient sensor dropouts)
        // 4. Exactly one alert dispatched per continuous distress bout
        // 5. Cooldown has elapsed since last Discord notification (45s)
        if (duration_ms >= DISTRESS_TIME_THRESHOLD_MS &&
            distress_density >= 0.50f &&
            s_distress_sample_count >= 10 &&
            !s_alert_sent_for_current_bout) {
            if (s_last_alert_time == 0 || (now - s_last_alert_time >= ALERT_COOLDOWN_MS)) {
                trigger = true;
                s_alert_sent_for_current_bout = true;
                s_last_alert_time = now;
                Serial.printf("[Stress Detector] *** ALERT QUALIFIED! Dominant: %s (Angry: %u, Scared: %u), Density: %.0f%% ***\n",
                    getEmotionName(dominant_emotion), s_angry_count, s_scared_count, distress_density * 100.0f);
            }
        }

        // Calculate average confidence for the dominant emotion
        if (dominant_emotion == EMOTION_ANGRY && s_angry_count > 0) {
            avg_conf = s_angry_conf_sum / (float)s_angry_count;
        } else if (dominant_emotion == EMOTION_SCARED && s_scared_count > 0) {
            avg_conf = s_scared_conf_sum / (float)s_scared_count;
        } else if (s_distress_sample_count > 0) {
            avg_conf = (s_angry_conf_sum + s_scared_conf_sum) / (float)s_distress_sample_count;
        }
    }

    if (report != nullptr) {
        report->stress_index = s_smoothed_stress;
        report->raw_sample_score = s_latest_raw_score;
        report->consecutive_distress_sec = current_duration_sec;
        report->dominant_emotion = dominant_emotion;
        report->avg_confidence = avg_conf;
        report->css_level = getEstimatedCSSLevel();
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
    if (!s_bout_active) return 0.0f;
    uint32_t now = millis();
    if (now - s_last_distress_frame_ms >= BOUT_GAP_TOLERANCE_MS) return 0.0f;
    return (float)(now - s_bout_start_time_ms) / 1000.0f;
}

uint32_t getAlertCooldownRemainingSec() {
    if (s_last_alert_time == 0) return 0;
    uint32_t now = millis();
    if (now - s_last_alert_time >= ALERT_COOLDOWN_MS) return 0;
    return (ALERT_COOLDOWN_MS - (now - s_last_alert_time)) / 1000;
}

void resetAlertCooldown() {
    s_last_alert_time = 0;
    s_alert_sent_for_current_bout = false;
}
