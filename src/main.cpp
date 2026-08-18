#include <Arduino.h>

#include "gateway_app.hpp"

namespace {
gathra::gateway::GatewayApp app;
}

void setup() { app.begin(); }
void loop() { app.loop(); }
