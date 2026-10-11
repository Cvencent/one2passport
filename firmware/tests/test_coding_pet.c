#include "coding_pet.h"
#include <assert.h>

int main(void)
{
    coding_pet_t pet = {0};
    assert(coding_pet_state(&pet) == CODING_PET_OFFLINE);
    pet.connected = true;
    assert(coding_pet_state(&pet) == CODING_PET_IDLE);
    coding_pet_update(&pet, 2, 1, 1);
    assert(coding_pet_state(&pet) == CODING_PET_BUSY);
    coding_pet_update(&pet, 0, 1, 1);
    assert(coding_pet_state(&pet) == CODING_PET_FAIL);
    pet.connected = false;
    assert(coding_pet_state(&pet) == CODING_PET_FAIL);
    coding_pet_update(&pet, 0, 1, 0);
    assert(coding_pet_state(&pet) == CODING_PET_DONE);
    coding_pet_update(&pet, 0, 0, 0);
    assert(coding_pet_state(&pet) == CODING_PET_OFFLINE);
    pet.connected = true;
    assert(coding_pet_state(&pet) == CODING_PET_IDLE);
    return 0;
}
