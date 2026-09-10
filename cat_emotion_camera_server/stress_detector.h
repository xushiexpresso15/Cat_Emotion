#pragma once
#include <Arduino.h>

enum EmotionClass {
    EMOTION_ANGRY = 0,
    EMOTION_FOCUS = 1,
    EMOTION_RELAX = 2,
    EMOTION_SCARED = 3,
    EMOTION_NONE = -1
};

struct StressReport {
    float stress_index;               // Smoothed stress score (0.0 to 1.0)
    float raw_sample_score;           // Latest instantaneous score
    float consecutive_distress_sec;   // Seconds of continuous high distress
    int   dominant_emotion;           // EMOTION_SCARED or EMOTION_ANGRY
    float avg_confidence;             // Confidence percentage (0.0 to 1.0)
    bool  is_alert_triggered;         // True if threshold met and cooldown expired
};

void initStressDetector();
void updateStressSample(int emotion_class, float confidence);
bool evaluateStressAnomaly(StressReport* report);
float getCurrentStressScore();
const char* getEmotionName(int emotion_class);
