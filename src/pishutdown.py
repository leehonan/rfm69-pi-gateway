# script to implement RPi shutdown and reboot on GPIO-connected button press
from subprocess import call
import RPi.GPIO as GPIO
import os
import time

# Define a function to run when an interrupt is called
def shutdown(pin):
    print("shutting down")
    os.system("sudo shutdown -h now") # Shutdown command

def reboot(pin):
    print("rebooting")
    os.system("sudo reboot") # Reboot command

GPIO.setmode(GPIO.BCM) # Set pin numbering to board numbering
GPIO.setup(23, GPIO.IN)		# already have pullup
GPIO.add_event_detect(23, GPIO.FALLING, callback=shutdown, bouncetime=200) #Set up an interrupt to look for button presses
GPIO.setup(24, GPIO.IN)         # already have pullup
GPIO.add_event_detect(24, GPIO.FALLING, callback=reboot, bouncetime=200) #Set up an interrupt to look for button presses

while True:
    time.sleep(.5)
