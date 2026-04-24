#pragma once

struct PatientInfo {
    char name[128];
    int  sex;       // 1=male 0=female
    int  dob_year;
    int  dob_month;
    int  dob_day;
};

PatientInfo load_patient(const char* path);
