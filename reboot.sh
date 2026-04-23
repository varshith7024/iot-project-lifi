#!/bin/bash

FQBN="esp32:esp32:esp32"

echo "Compiling in parallel..."

arduino-cli compile --fqbn $FQBN lifi_reciever/ &
C1=$!

arduino-cli compile --fqbn $FQBN lifi_transmitter/ &
C2=$!

wait $C1
wait $C2

echo "Uploading in parallel..."

arduino-cli upload -p /dev/ttyUSB0 --fqbn $FQBN lifi_reciever/ &
U1=$!

arduino-cli upload -p /dev/ttyUSB1 --fqbn $FQBN lifi_transmitter/ &
U2=$!

wait $U1
wait $U2

echo "Done."
