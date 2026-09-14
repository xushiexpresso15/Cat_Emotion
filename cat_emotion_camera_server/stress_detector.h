#pragma once
#include <Arduino.h>

enum EmotionClass {
    EMOTION_ANGRY = 0,
    EMOTION_FOCUS = 1,
    EMOTION_RELAX = 2,
    EMOTION_SCARED = 3,
    EMOTION_NONE = -1
};

// Research-backed Feline Stress Assessment Report
// Grounded in:
// - Kessler & Turner (1997) Cat Stress Score (CSS 1-7)
// - Evangelista et al. (2019) Feline Grimace Scale (FGS)
// - Caeiro, Burrows & Waller (2017) CatFACS (Action Units 105, 43, 109)
// - Stella, Croney & Buffington (2013) Environmental Stress Dynamics
// - Yeon et al. (2002) Autonomic Sympathetic Escalation
struct StressReport {
    float stress_index;               // Smoothed stress score (0.0 to 1.0)
    float raw_sample_score;           // Latest instantaneous score
    float consecutive_distress_sec;   // Seconds of continuous high distress
    int   dominant_emotion;           // EMOTION_SCARED or EMOTION_ANGRY
    float avg_confidence;             // Confidence percentage (0.0 to 1.0)
    int   css_level;                  // Estimated Cat Stress Score (1 to 7)
    bool  is_alert_triggered;         // True if threshold met and cooldown expired
};

void initStressDetector();
void updateStressSample(int emotion_class, float confidence);
bool evaluateStressAnomaly(StressReport* report);
bool isDistressBoutActive();
float getCurrentStressScore();
int getEstimatedCSSLevel();
float getConsecutiveDistressSec();
uint32_t getAlertCooldownRemainingSec();
void resetAlertCooldown();
const char* getEmotionName(int emotion_class);
const char* getCSSLevelDescription(int css_level);
