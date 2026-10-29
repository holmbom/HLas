#include "HLas.h"

int test_sidecar();
int test_sidecar_lite();

int main() {
    // Lite-formatet behöver ingen testdata alls — det bygger sina egna punkter.
    return test_sidecar_lite() + test_sidecar();
}
