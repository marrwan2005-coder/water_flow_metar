// Marrwan
#include <LiquidCrystal_I2C.h>
#include <Wire.h> // For I2C LCD

#define FLOW_SENSOR_PIN 2 // Interrupt-capable pin
#define LCD_I2C_ADDRESS 0x27

// YF-S201 factory spec: pulse frequency (Hz) = CALIBRATION_FACTOR * flow rate (L/min).
// Cheap/clone sensors often deviate from the datasheet value - run calibration
// mode (send 'c' over Serial) to measure your actual sensor's factor and
// update this constant with the result.
#define CALIBRATION_FACTOR 7.5

// Noise/contact-bounce filter: ignore pulses arriving closer together than this.
// The YF-S201 tops out around 225 Hz (~4.4 ms/pulse) at its 30 L/min rated max,
// so anything faster than 1 ms apart is electrical noise, not a real pulse.
#define MIN_PULSE_INTERVAL_US 1000

// How often the display/serial refresh, in milliseconds.
#define UPDATE_INTERVAL_MS 1000

// Calibration window length. Longer = more accurate average, but you need to
// be able to hold steady flow and measure volume for the whole window.
#define CAL_DURATION_MS 30000

volatile unsigned long flow_pulse_count = 0;
volatile unsigned long last_pulse_time_us = 0;

float flow_rate_L_hour = 0.0;
float total_volume_L = 0.0;
unsigned long lastUpdateTime = 0;

LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);

bool calibrating = false;
unsigned long calStartMillis = 0;

void setup()
{
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();

  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flow, RISING);

  lcd.setCursor(0, 0); // Initial LCD display
  lcd.print("Water Flow Meter");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  Serial.println(F("Ready. Send 'c' to start a 30s calibration window."));
  lastUpdateTime = millis();
}

void loop()
{
  if (Serial.available() && Serial.read() == 'c' && !calibrating)
  {
    startCalibration();
  }

  if (calibrating)
  {
    runCalibration();
    return; // suspend normal flow display while calibrating
  }

  unsigned long now = millis();
  unsigned long dt = now - lastUpdateTime;
  if (dt >= UPDATE_INTERVAL_MS)
  {
    lastUpdateTime = now;

    // Snapshot and reset the pulse count atomically so the ISR can't
    // increment it mid-read/mid-reset and drop or double count a pulse.
    noInterrupts();
    unsigned long pulses = flow_pulse_count;
    flow_pulse_count = 0;
    interrupts();

    // Frequency from the ACTUAL elapsed time, not an assumed 1.000 s window.
    float freq_hz = (float)pulses * 1000.0 / (float)dt;

    // K-factor spec: freq(Hz) = K * flow(L/min)  ->  flow(L/min) = freq / K
    flow_rate_L_hour = (freq_hz / CALIBRATION_FACTOR) * 60.0;

    // Exact volume: each pulse represents 1 / (K * 60) liters, independent of
    // timing, so summing pulses avoids the drift of rate/3600 per loop.
    total_volume_L += (float)pulses / (CALIBRATION_FACTOR * 60.0);

    updateDisplay();

    Serial.print(F("Flow Rate: "));
    Serial.print(flow_rate_L_hour, 2);
    Serial.print(F(" L/hour  Total: "));
    Serial.print(total_volume_L, 2);
    Serial.println(F(" L"));
  }
}

// Overwrites a full 16-char LCD row without lcd.clear(), so the display does
// not flicker on every refresh. Pads the remainder of the line with spaces.
void lcdPrintLine(uint8_t row, const char *text)
{
  lcd.setCursor(0, row);
  uint8_t i = 0;
  while (text[i] != '\0' && i < 16)
  {
    lcd.print(text[i]);
    i++;
  }
  while (i < 16)
  {
    lcd.print(' ');
    i++;
  }
}

void updateDisplay()
{
  char line[24];
  char num[12];

  dtostrf(flow_rate_L_hour, 0, 1, num); // value with 1 decimal place
  snprintf(line, sizeof(line), "Flow: %s L/h", num);
  lcdPrintLine(0, line);

  dtostrf(total_volume_L, 0, 1, num);
  snprintf(line, sizeof(line), "Total: %s L", num);
  lcdPrintLine(1, line);
}

void startCalibration()
{
  calibrating = true;
  calStartMillis = millis();

  noInterrupts();
  flow_pulse_count = 0;
  interrupts();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Calibrating...");

  Serial.println(F("Calibration started."));
  Serial.println(F("Start collecting the sensor's output into a measuring"));
  Serial.println(F("container NOW. Window length: 30 seconds."));
}

void runCalibration()
{
  unsigned long elapsed = millis() - calStartMillis;

  lcd.setCursor(0, 1);
  lcd.print("t=");
  lcd.print(elapsed / 1000);
  lcd.print("s   ");

  if (elapsed < CAL_DURATION_MS)
  {
    return;
  }

  noInterrupts();
  unsigned long pulses = flow_pulse_count;
  flow_pulse_count = 0; // clear so normal mode doesn't inherit these pulses
  interrupts();

  float elapsed_s = elapsed / 1000.0;
  float freq_hz = pulses / elapsed_s;

  Serial.println(F("=== Calibration window complete ==="));
  Serial.print(F("Elapsed: "));
  Serial.print(elapsed_s, 2);
  Serial.println(F(" s"));
  Serial.print(F("Pulses counted: "));
  Serial.println(pulses);
  Serial.print(F("Average frequency: "));
  Serial.print(freq_hz, 3);
  Serial.println(F(" Hz"));
  Serial.println();
  Serial.println(F("Now enter the ACTUAL volume you collected during this"));
  Serial.println(F("window, in liters (e.g. 0.85), then press Enter:"));

  float measured_volume_L = waitForSerialFloat();

  if (measured_volume_L <= 0.0)
  {
    Serial.println(F("Invalid volume (must be > 0). Calibration aborted."));
    calibrating = false;
    lastUpdateTime = millis();
    return;
  }

  float flow_L_min = measured_volume_L / (elapsed_s / 60.0);
  float k_factor = freq_hz / flow_L_min;

  Serial.println();
  Serial.print(F("Measured flow: "));
  Serial.print(flow_L_min, 3);
  Serial.println(F(" L/min"));
  Serial.print(F("Your calibration factor (K): "));
  Serial.println(k_factor, 3);
  Serial.println(F("Update CALIBRATION_FACTOR at the top of the sketch with this value."));
  Serial.println(F("Send 'c' again to run another test point at a different flow rate."));

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("K factor:");
  lcd.setCursor(0, 1);
  lcd.print(k_factor, 3);

  calibrating = false;
  lastUpdateTime = millis(); // resume normal display cadence cleanly
}

// Blocks until a line is typed into the Serial Monitor and Enter is pressed.
float waitForSerialFloat()
{
  while (!Serial.available())
  {
    // wait for the user to type the measured volume
  }
  delay(50); // let the rest of the line arrive before reading it
  String line = Serial.readStringUntil('\n');
  return line.toFloat();
}

void flow()
{
  unsigned long now = micros();
  if (now - last_pulse_time_us < MIN_PULSE_INTERVAL_US)
  {
    return; // reject pulses that arrive faster than physically possible - noise
  }
  last_pulse_time_us = now;
  flow_pulse_count++;
}
