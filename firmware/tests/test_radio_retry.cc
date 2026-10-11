#include "radio_retry.h"
#include <cassert>
int main() {
    assert(radio_retry_cancelled(true, true, 7, 8, 2, 2));
    assert(radio_retry_cancelled(true, true, 7, 7, 2, 3));
    assert(radio_retry_cancelled(false, true, 7, 7, 2, 2));
    assert(radio_retry_cancelled(true, false, 7, 7, 2, 2));
    assert(!radio_retry_cancelled(true, true, 7, 7, 2, 2));
}
