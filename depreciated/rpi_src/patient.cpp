#include "patient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

PatientInfo load_patient(const char* path) {
    PatientInfo p = {"Unknown", 0, 1900, 1, 1};
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Warning: cannot open %s, using defaults\n", path);
        return p;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[192];
        // %63[^= ] = key: up to 63 chars that are not '=' or space; then literal " = "; %191[^\n] = value: rest of line
        if (sscanf(line, " %63[^= ] = %191[^\n]", key, val) != 2) continue;
        if      (strcmp(key, "name")      == 0) strncpy(p.name, val, sizeof(p.name) - 1);
        else if (strcmp(key, "sex")       == 0) p.sex       = (strcmp(val, "male") == 0) ? 1 : 0;
        else if (strcmp(key, "dob_year")  == 0) p.dob_year  = atoi(val);
        else if (strcmp(key, "dob_month") == 0) p.dob_month = atoi(val);
        else if (strcmp(key, "dob_day")   == 0) p.dob_day   = atoi(val);
    }
    fclose(f);
    return p;
}
