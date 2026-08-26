#pragma once

struct WorldEntity {
    enum Type { Caravan, AnimalGroup, Army };

    int size;
    float strength;
    float temperatureResistance;
};