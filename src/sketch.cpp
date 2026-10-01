#include <Arduino.h>            // Arduino basics: pinMode, digitalWrite, millis, tone, Serial
/*
 *  Hardware : ESP32, 4x4 keypad, SSD1306 OLED, MFRC522 RFID reader,
 *             servo motor, buzzer, green/red LEDs, door switch
 *
 *  1. Tap RFID card -> 2. Type PIN, # to submit -> 3. Servo unlocks
 *  3 failures              -> 30 second lockout
 *  Door forced open        -> tamper alarm
 *  Admin card + PIN        -> admin menu
 *  Cloud: logs and status go to Firebase (web dashboard);
 *         alarms and lockouts send a phone alert (ntfy app).
 */

// 1. LIBRARIES AND PREPROCESSOR (runs before compiling)
#include <Wire.h>              // I2C bus: talks to the OLED on 2 wires
#include <SPI.h>               // SPI bus: talks to the RFID reader
#include <Keypad.h>            // reads which key is pressed on the 4x4 pad
#include <ESP32Servo.h>        // moves the servo (the lock bolt)
#include <Adafruit_GFX.h>      // drawing text and lines on a screen
#include <Adafruit_SSD1306.h>  // driver for our OLED screen
#include <MFRC522.h>           // reads RFID card IDs
#include <LittleFS.h>          // file system in ESP32 flash (log file)
#include <WiFi.h>              // connect to Wi-Fi
#include <HTTPClient.h>        // send web requests (Firebase, ntfy)
#include <WiFiClientSecure.h>  // HTTPS = encrypted web requests
#include <stdio.h>             // C: fopen, fprintf, fgets, fclose, snprintf
#include <stdlib.h>            // C: malloc, free (dynamic memory)
#include <string.h>            // C: strcmp, strncpy (strings)
#include <time.h>              // C: time() gives real date and time

// #define = a name the preprocessor replaces with a value before compiling
#define PIN_SERVO        13    // servo signal wire is on GPIO 13
#define PIN_BUZZER       15    // buzzer on GPIO 15
#define PIN_DOOR         34    // door switch on GPIO 34 (input only pin)
#define PIN_LED_GREEN     2    // green LED = unlocked
#define PIN_LED_RED      12    // red LED = locked / blinking alarm
#define PIN_RFID_SS       5    // RFID "chip select" pin
#define PIN_RFID_RST     27    // RFID reset pin

#define SERVO_LOCKED      0    // servo angle when locked (degrees)
#define SERVO_UNLOCKED   90    // servo angle when unlocked
#define DOOR_CLOSED_LEVEL LOW    // change to HIGH if door reads reversed

#define MAX_USERS           5  // size of the users array
#define PIN_LENGTH          4  // PINs have 4 digits
#define MAX_NAME_LENGTH    12  // max characters in a name, incl. '\0'
#define MAX_FAILED          3  // 3 failures in a row = lockout
#define MAX_LOGS_IN_MEMORY 20  // linked list keeps only the newest 20

#define PIN_TIMEOUT_MS    15000UL  // 15 s to type the PIN (UL = unsigned long, like millis)
#define LOCKOUT_MS        30000UL  // lockout lasts 30 s
#define UNLOCK_WINDOW_MS  10000UL  // relock after 10 s if door never opened
#define RELOCK_DELAY_MS    1000UL  // relock 1 s after the door closes
#define ADMIN_TIMEOUT_MS  30000UL  // leave admin mode after 30 s idle
#define MESSAGE_MS         2000UL  // short messages stay 2 s on the OLED
#define NET_INTERVAL_MS    2000UL   // talk to the cloud every 2 s

#define LOG_FILE_PATH    "/littlefs/access_log.txt"   // log file in flash
#define WIFI_SSID        "Wokwi-GUEST"                // Wokwi's free Wi-Fi
#define WIFI_PASSWORD    ""                           // no password needed
#define FIREBASE_URL     "https://YOUR-PROJECT-default-rtdb.firebaseio.com"   // your Firebase Realtime Database URL
#define NTFY_TOPIC       "your-unique-topic-name"     // ntfy topic for phone alerts
#define ALERT_QUEUE_SIZE  5    // alert queue holds 5 messages
#define ALERT_LENGTH     80    // each alert up to 80 characters

// Macro with parameters: seconds left on a timer, never below 0 (\ = continues on next line)
#define SECONDS_LEFT(elapsed, total) \
    ((elapsed) < (total) ? ((total) - (elapsed)) / 1000UL : 0UL)

// Conditional compilation: DEBUG 1 = prints on, DEBUG 0 = prints removed
#define DEBUG 1
#if DEBUG                                         // if DEBUG is 1 ...
  #define DEBUG_PRINT(text) Serial.println(text)  // ... DEBUG_PRINT prints
#else                                             // otherwise ...
  #define DEBUG_PRINT(text)                       // ... it becomes nothing
#endif


// 2. DATA TYPES (our own types, made with typedef)

// Union: both members share the SAME 4 bytes in memory
typedef union {
    uint8_t  bytes[4];   // the ID as 4 separate bytes (how the reader gives it)
    uint32_t value;      // the same 4 bytes as one number (compare with ==)
} CardID;                // size is 4 bytes, not 8

// Structure: groups everything about one user
typedef struct {
    int    id;                        // user number 0..4
    char   name[MAX_NAME_LENGTH];     // name as a C string
    char   pin[PIN_LENGTH + 1];       // +1 for the '\0'
    CardID card;                      // value 0 = no card
    bool   active;                    // false = disabled by admin
    bool   is_admin;                  // true only for the Admin user
} User;

typedef enum {         // enum: names that C numbers 0, 1, 2 ... automatically
    EV_BOOT, EV_GRANTED, EV_WRONG_PIN, EV_UNKNOWN_CARD, EV_DISABLED_USER,        // 0-4
    EV_PIN_TIMEOUT, EV_LOCKOUT, EV_TAMPER, EV_ALARM_CLEARED, EV_DOOR_OPENED,     // 5-9
    EV_DOOR_CLOSED, EV_RELOCKED, EV_CARD_ENROLLED, EV_USER_ENABLED, EV_USER_DISABLED  // 10-14
} EventType;           // every kind of event the log can record

// Array of strings, same order as the enum: event_names[EV_TAMPER] is "TAMPER_ALARM"
const char *event_names[] = {
    "BOOT", "ACCESS_GRANTED", "WRONG_PIN", "UNKNOWN_CARD", "DISABLED_USER",
    "PIN_TIMEOUT", "LOCKOUT", "TAMPER_ALARM", "ALARM_CLEARED", "DOOR_OPENED",
    "DOOR_CLOSED", "RELOCKED", "CARD_ENROLLED", "USER_ENABLED", "USER_DISABLED"
};

// Structure with a pointer to itself = one node of a linked list
typedef struct LogEntry {
    unsigned long    number;          // log number 1, 2, 3 ...
    time_t           timestamp;       // real date/time, 0 if unknown
    bool             uploaded;        // already sent to the cloud?
    EventType        type;            // what happened (enum)
    char             user_name[MAX_NAME_LENGTH];  // who, or "-"
    struct LogEntry *next;            // NULL at the end of the list
} LogEntry;

// The states of the locker (state machine): always exactly one of these
typedef enum {
    ST_IDLE, ST_WAIT_PIN, ST_UNLOCKED, ST_LOCKOUT,       // 0-3: normal use
    ST_ADMIN_MENU, ST_ADMIN_PICK, ST_ADMIN_CARD,         // 4-6: admin mode
    NUM_STATES                        // = 7, counts the states above
} State;

// What happened in one pass of loop()
typedef struct {
    char          key;                // 0 = no key pressed
    bool          card_tapped;        // true if a card was read
    CardID        card;               // the card's ID (if tapped)
    unsigned long now;                // time in ms since power-on
} Event;

// Function pointer type: "a function that takes an Event pointer and returns nothing"
typedef void (*StateHandler)(const Event *ev);


// 3. HARDWARE OBJECTS
char key_layout[4][4] = {             // 2D array: 4 rows x 4 columns of keys
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
byte row_pins[4] = {32, 33, 25, 26};  // GPIO pins for the 4 keypad rows
byte col_pins[4] = {14, 16, 17, 4};   // GPIO pins for the 4 keypad columns

Keypad           keypad = Keypad(makeKeymap(key_layout), row_pins, col_pins, 4, 4);  // keypad object
Adafruit_SSD1306 display(128, 64, &Wire, -1);   // OLED, 128x64 pixels, on I2C
MFRC522          rfid(PIN_RFID_SS, PIN_RFID_RST);  // RFID reader object
Servo            lock_servo;                    // servo object
WiFiClientSecure secure_client;                 // HTTPS connection
HTTPClient       http;                          // sends web requests


/* 4. GLOBAL DATA
 * Storage class: "static" globals are private to this file
 */
static User users[MAX_USERS];         // array of structures
static int  user_count = 0;           // how many users are added

static LogEntry *log_head  = NULL;    // oldest entry
static LogEntry *log_tail  = NULL;    // newest entry
static int       log_count = 0;       // entries in the list now
static bool      file_ok   = false;   // true if the file system works
static bool      wifi_ok   = false;   // true if Wi-Fi connected at start

static State         state       = ST_IDLE;  // current state of the machine
static unsigned long state_since = 0;        // when we entered this state (ms)

static User *current_user = NULL;     // pointer to a structure
static User *admin_target = NULL;     // user the admin is changing
static char  admin_action = 0;        // 'A' = enroll card, 'B' = enable/disable

static char pin_entry[PIN_LENGTH + 1];  // digits typed so far, as a string
static int  pin_length = 0;             // how many digits typed
static int  failed_attempts = 0;        // failures in a row

static bool locked       = true;      // is the bolt locked?
static bool alarm_active = false;     // tamper alarm on?
static bool buzzer_on    = false;     // is the buzzer sounding now?

static bool          door_closed = true;                 // door switch state
static bool          door_opened_while_unlocked = false; // normal use happened
static unsigned long door_closed_since = 0;              // when door last closed

static char          message_big[12];     // big line of a short message
static char          message_small[22];   // small line of a short message
static unsigned long message_until = 0;   // show message until this time

// 2D char array used as a circular queue of phone alerts
static char alert_queue[ALERT_QUEUE_SIZE][ALERT_LENGTH];  // 5 strings of 80 chars
static int  alert_head  = 0;          // index of the oldest alert
static int  alert_count = 0;          // alerts waiting to be sent


// 5. FUNCTION PROTOTYPES (declared here, defined below)
void log_event(EventType type, const User *user);   // used before it is written
void queue_alert(const char *text);                 // used before it is written


// 6. SMALL HELPERS
void set_state(State new_state)       // change state and note the time
{
    state = new_state;
    state_since = millis();           // millis() = ms since power-on
}

// Strings: copy safely and always end with '\0'
void copy_text(char *dest, const char *src, int size)
{
    strncpy(dest, src, size - 1);     // copy at most size-1 chars (no overflow)
    dest[size - 1] = '\0';            // strncpy may skip '\0', so add it
}

// Shows a two-line message for 2 seconds
void show_message(const char *big, const char *small)
{
    copy_text(message_big, big, sizeof(message_big));        // sizeof = array size
    copy_text(message_small, small, sizeof(message_small));
    message_until = millis() + MESSAGE_MS;                    // expires in 2 s
}

void clear_pin_entry(void)            // forget the typed digits
{
    pin_length = 0;
    pin_entry[0] = '\0';              // empty string
}

void beep_click(void) { tone(PIN_BUZZER, 1500, 30);  }   // key press: 1500 Hz, 30 ms
void beep_ok(void)    { tone(PIN_BUZZER, 2500, 200); }   // success: high beep
void beep_error(void) { tone(PIN_BUZZER, 400, 500);  }   // error: low long beep

// Pointer arithmetic: walk through the bytes with p++
void print_card_id(const uint8_t *p, int length)
{
    while (length > 0) {
        Serial.printf("%02X ", *p);   // value at address p
        p++;                          // next byte
        length--;
    }
    Serial.println();                 // new line at the end
}

CardID make_card(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)  // builds a CardID
{
    CardID c = {{b0, b1, b2, b3}};    // fill the bytes[] member
    return c;                         // returns the whole union
}


// 7. USERS
void add_user(const char *name, const char *pin, CardID card, bool is_admin)
{
    if (user_count >= MAX_USERS) return;   // array full: do nothing

    User *u = &users[user_count];     // pointer to the next free slot
    u->id = user_count;               // -> = member through a pointer
    copy_text(u->name, name, MAX_NAME_LENGTH);
    copy_text(u->pin, pin, PIN_LENGTH + 1);
    u->card = card;
    u->active = true;                 // new users start enabled
    u->is_admin = is_admin;
    user_count++;                     // one more user
}

/* Wokwi's built-in cards: Blue 01020304, Green 11223344, Yellow 55667788,
 * Red AABBCCDD (not registered = "unknown card"), Key Fob C0FFEE99 */
void setup_users(void)
{
    add_user("Admin",   "0000", make_card(0xC0, 0xFF, 0xEE, 0x99), true);   // id 0
    add_user("Alice",   "1234", make_card(0x01, 0x02, 0x03, 0x04), false);  // id 1
    add_user("Bob",     "5678", make_card(0x11, 0x22, 0x33, 0x44), false);  // id 2
    add_user("Charlie", "2468", make_card(0x55, 0x66, 0x77, 0x88), false);  // id 3
    add_user("Guest",   "1111", make_card(0, 0, 0, 0), false);   // card added by admin
}

// Returns a pointer to the matching user, or NULL
User *find_user_by_card(CardID card)
{
    for (int i = 0; i < user_count; i++) {       // check every user
        if (users[i].card.value == card.value) return &users[i];   // union: one compare
    }
    return NULL;                      // no match = unknown card
}

// Recursion: binary search by id (users are sorted by id)
User *find_user_by_id(int id, int low, int high)
{
    if (low > high) return NULL;                       // base case: not found

    int mid = (low + high) / 2;                        // middle of the range
    if (users[mid].id == id) return &users[mid];       // base case: found
    if (users[mid].id < id)  return find_user_by_id(id, mid + 1, high);  // search right half
    return find_user_by_id(id, low, mid - 1);          // search left half
}


// 8. ACCESS LOG (linked list in memory + text file in flash)

// Files: append one line to the log file
void save_log_to_file(const LogEntry *e)
{
    if (!file_ok) return;                    // no file system: skip
    FILE *f = fopen(LOG_FILE_PATH, "a");     // "a" = add to the end; *f is a file pointer
    if (f == NULL) return;                   // could not open: skip
    fprintf(f, "%lu,%ld,%s,%s\n", e->number, (long)e->timestamp,   // write one CSV line
            event_names[e->type], e->user_name);
    fclose(f);                               // always close the file
}

// Files: read the log file line by line
void print_log_file(void)
{
    FILE *f = fopen(LOG_FILE_PATH, "r");     // "r" = read
    if (f == NULL) {
        Serial.println("(no log file)");
        return;
    }
    char line[64];                           // buffer for one line
    while (fgets(line, sizeof(line), f) != NULL) Serial.print(line);  // NULL = end of file
    fclose(f);
}

// Recursion on a linked list: count = 1 + count of the rest
int count_logs(const LogEntry *node)
{
    if (node == NULL) return 0;              // base case: end of list
    return 1 + count_logs(node->next);       // this node + the rest
}

// Dynamic memory: malloc() a new node, free() the oldest
void log_event(EventType type, const User *user)
{
    static unsigned long next_number = 1;    // static local keeps its value

    LogEntry *entry = (LogEntry *)malloc(sizeof(LogEntry));   // ask for memory for 1 node
    if (entry == NULL) {                     // NULL = no memory left
        DEBUG_PRINT("Out of memory: log not saved");
        return;
    }
    entry->number    = next_number++;        // use number, then add 1
    entry->timestamp = time(NULL);           // real time from NTP
    if (entry->timestamp < 1000000000) entry->timestamp = 0;   // clock not set yet
    entry->uploaded  = false;                // not sent to Firebase yet
    entry->type      = type;
    entry->next      = NULL;                 // it will be the last node
    copy_text(entry->user_name, user != NULL ? user->name : "-", MAX_NAME_LENGTH);  // "-" if no user

    // add to the end of the list
    if (log_tail == NULL) log_head = entry;  // empty list: first node
    else                  log_tail->next = entry;  // link after the last node
    log_tail = entry;                        // new node is now the last
    log_count++;

    // keep only the newest entries in memory (the file keeps all)
    if (log_count > MAX_LOGS_IN_MEMORY) {
        LogEntry *oldest = log_head;         // remember the first node
        log_head = log_head->next;           // list now starts at the 2nd
        free(oldest);                        // give its memory back
        log_count--;
    }

    Serial.printf("[LOG #%lu] %-14s user=%s\r\n", entry->number,   // print to Serial Monitor
                  event_names[type], entry->user_name);
    save_log_to_file(entry);                 // also save to the file

    // phone alerts for security events
    if (type == EV_TAMPER) {
        queue_alert("Tamper alarm! The locker door was opened while locked.");
    } else if (type == EV_LOCKOUT) {
        char alert[ALERT_LENGTH];
        snprintf(alert, sizeof(alert), "%d failed attempts (last: %s). Locked for %lu s.",   // build text safely
                 MAX_FAILED, user != NULL ? user->name : "unknown card", LOCKOUT_MS / 1000UL);
        queue_alert(alert);                  // send it later
    }
}


// 9. LOCK CONTROL
void set_lock(bool lock)              // true = lock, false = unlock
{
    locked = lock;
    lock_servo.write(lock ? SERVO_LOCKED : SERVO_UNLOCKED);   // ternary: 0 or 90 degrees
}

void unlock_locker(void)
{
    set_lock(false);                  // servo to 90
    door_opened_while_unlocked = false;
    set_state(ST_UNLOCKED);
    beep_ok();
    show_message("WELCOME", current_user != NULL ? current_user->name : "");
}

void lock_locker(void)
{
    set_lock(true);                   // servo back to 0
    log_event(EV_RELOCKED, NULL);
    current_user = NULL;              // nobody is logged in now
    set_state(ST_IDLE);
}

// Called after every wrong PIN or unknown card
void register_failure(const char *reason)
{
    failed_attempts++;
    beep_error();

    if (failed_attempts >= MAX_FAILED) {     // 3rd failure: lockout
        log_event(EV_LOCKOUT, current_user);
        show_message("LOCKOUT", reason);
        set_state(ST_LOCKOUT);
    } else {                                 // show tries left
        char text[32];
        snprintf(text, sizeof(text), "%s (%d left)", reason, MAX_FAILED - failed_attempts);
        show_message("DENIED", text);
        set_state(ST_IDLE);
    }
    current_user = NULL;
}

void check_pin(void)
{
    if (strcmp(pin_entry, current_user->pin) == 0) {   // strcmp: 0 = equal
        failed_attempts = 0;                 // correct PIN resets the count
        if (alarm_active) {                  // a valid user clears the alarm
            alarm_active = false;
            log_event(EV_ALARM_CLEARED, current_user);
        }
        if (current_user->is_admin) {        // admin goes to the menu
            beep_ok();
            set_state(ST_ADMIN_MENU);
        } else {                             // normal user: open
            log_event(EV_GRANTED, current_user);
            unlock_locker();
        }
    } else {                                 // wrong PIN
        log_event(EV_WRONG_PIN, current_user);
        register_failure("Wrong PIN");
    }
    clear_pin_entry();                       // always forget typed digits
}


// 10. READING INPUTS

// Returns true if a new card was tapped, and fills *card (call by reference)
bool read_card(CardID *card)
{
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;  // no card

    bool ok = (rfid.uid.size == 4);           // only 4-byte IDs fit our union
    if (ok) {
        const uint8_t *src = rfid.uid.uidByte;        // pointers: copy bytes
        for (int i = 0; i < 4; i++) card->bytes[i] = *(src + i);   // *(src+i) = src[i]
        Serial.print("Card tapped: ");
        print_card_id(card->bytes, 4);
    } else {
        Serial.println("Card type not supported (only 4-byte IDs)");
    }
    rfid.PICC_HaltA();                        // tell the card to stop
    rfid.PCD_StopCrypto1();                   // end communication
    return ok;
}

// Checks the door switch; raises the tamper alarm if forced open
void check_door(unsigned long now)
{
    bool closed_now = (digitalRead(PIN_DOOR) == DOOR_CLOSED_LEVEL);   // read the switch
    if (closed_now == door_closed) return;         // nothing changed

    door_closed = closed_now;                      // remember new state
    if (door_closed) {                             // door just closed
        door_closed_since = now;
        log_event(EV_DOOR_CLOSED, NULL);
        return;
    }
    log_event(EV_DOOR_OPENED, NULL);               // door just opened
    if (state == ST_UNLOCKED) {
        door_opened_while_unlocked = true;         // normal use
    } else if (locked && !alarm_active) {
        alarm_active = true;                       // forced open!
        log_event(EV_TAMPER, NULL);
    }
}


// 11. STATE HANDLERS - one function per state
void state_idle(const Event *ev)              // locked, waiting for a card
{
    if (ev->card_tapped) {
        User *u = find_user_by_card(ev->card);
        if (u == NULL) {                      // unknown card
            log_event(EV_UNKNOWN_CARD, NULL);
            register_failure("Unknown card");
        } else if (!u->active) {              // disabled user
            log_event(EV_DISABLED_USER, u);
            beep_error();
            show_message("DENIED", "Card disabled");
        } else {                              // known card: ask for PIN
            current_user = u;
            clear_pin_entry();
            tone(PIN_BUZZER, 2000, 80);
            set_state(ST_WAIT_PIN);
        }
    } else if (ev->key != 0) {                // key pressed without a card
        show_message("LOCKED", "Tap card first");
    }
}

// Collect PIN digits: # to submit, * to clear
void state_wait_pin(const Event *ev)
{
    if (ev->now - state_since > PIN_TIMEOUT_MS) {   // more than 15 s passed
        log_event(EV_PIN_TIMEOUT, current_user);
        current_user = NULL;
        clear_pin_entry();
        show_message("TIMEOUT", "Tap card again");
        set_state(ST_IDLE);
        return;
    }
    char key = ev->key;
    if (key >= '0' && key <= '9' && pin_length < PIN_LENGTH) {   // a digit, and room left
        pin_entry[pin_length++] = key;        // store digit, then count +1
        pin_entry[pin_length] = '\0';         // keep it a valid string
    } else if (key == '*') {
        clear_pin_entry();
    } else if (key == '#') {
        check_pin();
    }
}

// Relock after the door closes, if nobody opens it, or on D
void state_unlocked(const Event *ev)
{
    unsigned long elapsed = ev->now - state_since;   // time since unlocking

    if ((!door_opened_while_unlocked && elapsed > UNLOCK_WINDOW_MS) ||   // never opened in 10 s
        (door_opened_while_unlocked && door_closed && ev->now - door_closed_since > RELOCK_DELAY_MS) ||  // closed for 1 s
        (ev->key == 'D' && door_closed)) {   // D pressed, door shut
        lock_locker();
    }
}

void state_lockout(const Event *ev)           // blocked: just wait
{
    if (ev->now - state_since > LOCKOUT_MS) {  // 30 s over
        failed_attempts = 0;
        set_state(ST_IDLE);
    }
}

// Leaves admin mode after 30 s with no action
bool admin_timed_out(const Event *ev)
{
    if (ev->now - state_since <= ADMIN_TIMEOUT_MS) return false;   // still in time
    current_user = NULL;
    set_state(ST_IDLE);
    return true;
}

// A enroll card, B enable/disable, C open, # logs, D exit
void state_admin_menu(const Event *ev)
{
    if (admin_timed_out(ev)) return;

    switch (ev->key) {                        // switch-case on the key pressed
        case 'A':                             // A and B share the same code
        case 'B':
            admin_action = ev->key;           // remember A or B
            set_state(ST_ADMIN_PICK);
            break;                            // break = leave the switch
        case 'C':
            log_event(EV_GRANTED, current_user);
            unlock_locker();
            break;
        case '#':
            print_log_file();
            Serial.printf("Log entries in memory: %d\r\n", count_logs(log_head));
            show_message("LOGS", "Sent to Serial");
            break;
        case 'D':
            current_user = NULL;
            set_state(ST_IDLE);
            break;
    }
}

// Admin types a user number
void state_admin_pick(const Event *ev)
{
    if (admin_timed_out(ev)) return;

    if (ev->key >= '1' && ev->key <= '9') {
        User *u = find_user_by_id(ev->key - '0', 0, user_count - 1);   // '3' -> 3
        if (u == NULL) {                      // no such user
            beep_error();
        } else if (admin_action == 'A') {     // enroll: wait for a card
            admin_target = u;
            set_state(ST_ADMIN_CARD);
        } else {                              // B: flip enabled/disabled
            u->active = !u->active;
            log_event(u->active ? EV_USER_ENABLED : EV_USER_DISABLED, u);
            show_message(u->active ? "ENABLED" : "DISABLED", u->name);
            set_state(ST_ADMIN_MENU);
        }
    } else if (ev->key == '*' || ev->key == 'D') {   // back to menu
        set_state(ST_ADMIN_MENU);
    }
}

// Admin taps the new card for the chosen user
void state_admin_card(const Event *ev)
{
    if (admin_timed_out(ev)) return;

    if (ev->card_tapped) {
        User *owner = find_user_by_card(ev->card);   // does someone own it?
        if (owner != NULL && owner != admin_target) {   // yes, someone else
            beep_error();
            show_message("IN USE", owner->name);
        } else {                                     // free: give it to the user
            admin_target->card = ev->card;
            log_event(EV_CARD_ENROLLED, admin_target);
            beep_ok();
            show_message("SAVED", admin_target->name);
            set_state(ST_ADMIN_MENU);
        }
    } else if (ev->key == '*') {                     // cancel
        set_state(ST_ADMIN_MENU);
    }
}

// Array of function pointers: the state number picks the function
StateHandler state_table[NUM_STATES] = {
    state_idle, state_wait_pin, state_unlocked, state_lockout,   // index 0-3
    state_admin_menu, state_admin_pick, state_admin_card         // index 4-6
};


// 12. OUTPUTS: LEDs, buzzer alarm, OLED screen
void update_leds_and_alarm(unsigned long now)
{
    bool blink = (now / 250) % 2 == 0;        // % 2 flips true/false every 250 ms

    digitalWrite(PIN_LED_GREEN, locked ? LOW : HIGH);   // green on when unlocked
    if (alarm_active || state == ST_LOCKOUT) digitalWrite(PIN_LED_RED, blink ? HIGH : LOW);  // red blinks
    else                                     digitalWrite(PIN_LED_RED, locked ? HIGH : LOW);  // red on when locked

    // alarm = fast beeping, lockout = slow beeping
    bool should_sound = false;
    int  frequency = 0;
    if (alarm_active) {
        should_sound = blink;                 // beep with the blink
        frequency = 2000;                     // high tone
    } else if (state == ST_LOCKOUT) {
        should_sound = (now % 1000) < 150;    // short beep once a second
        frequency = 800;                      // lower tone
    }

    if (should_sound && !buzzer_on) {         // start sound only once
        tone(PIN_BUZZER, frequency);
        buzzer_on = true;
    } else if (!should_sound && buzzer_on) {  // stop sound only once
        noTone(PIN_BUZZER);
        buzzer_on = false;
    }
}

void print_at(int x, int y, int size, const char *text)   // write text at x,y on the OLED
{
    display.setTextSize(size);
    display.setCursor(x, y);
    display.print(text);
}

void draw_screen(unsigned long now)           // redraws the whole OLED
{
    char line[32];                            // buffer for built text
    unsigned long elapsed = now - state_since;
    bool in_admin = (state >= ST_ADMIN_MENU); // enum values are numbers, so >= works

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // top bar
    print_at(0, 0, 1, in_admin ? "ADMIN MODE" : "SMART LOCKER");
    if (alarm_active) {
        if ((now / 400) % 2 == 0) print_at(98, 0, 1, "ALARM");   // flashing ALARM
    } else {
        print_at(110, 0, 1, locked ? "LCK" : "OPN");
    }
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);   // line under the top bar

    // a temporary message has priority
    if (now < message_until) {
        print_at(0, 18, 2, message_big);
        print_at(0, 44, 1, message_small);
        display.display();                    // send drawing to the screen
        return;
    }

    switch (state) {                          // each state has its own screen
        case ST_IDLE:
            print_at(0, 18, 2, "LOCKED");
            print_at(0, 44, 1, door_closed ? "Tap your card" : "Door is open!");
            break;

        case ST_WAIT_PIN:
            snprintf(line, sizeof(line), "Hi %s", current_user->name);   // "Hi Alice"
            print_at(0, 14, 1, line);
            print_at(0, 28, 2, "PIN:");
            for (int i = 0; i < pin_length; i++) display.print('*');   // hide digits as *
            snprintf(line, sizeof(line), "#=OK *=Clear  %2lus", SECONDS_LEFT(elapsed, PIN_TIMEOUT_MS));  // countdown
            print_at(0, 54, 1, line);
            break;

        case ST_UNLOCKED:
            print_at(0, 18, 2, "UNLOCKED");
            if (!door_opened_while_unlocked) {
                snprintf(line, sizeof(line), "Open door in %lus", SECONDS_LEFT(elapsed, UNLOCK_WINDOW_MS));
                print_at(0, 44, 1, line);
            } else {
                print_at(0, 44, 1, door_closed ? "Locking..." : "Close door to lock");
            }
            break;

        case ST_LOCKOUT:
            print_at(0, 18, 2, "LOCKOUT");
            snprintf(line, sizeof(line), "Try again in %lus", SECONDS_LEFT(elapsed, LOCKOUT_MS));
            print_at(0, 44, 1, line);
            break;

        case ST_ADMIN_MENU: {                 // { } needed to declare a variable inside a case
            const char *menu[5] = {"A: Enroll card", "B: Enable/disable",
                                   "C: Open locker", "#: Print logs", "D: Exit"};
            for (int i = 0; i < 5; i++) print_at(0, 14 + i * 10, 1, menu[i]);   // 10 px per row
            break;
        }

        case ST_ADMIN_PICK:
            print_at(0, 14, 1, admin_action == 'A' ? "Enroll: pick user" : "Toggle: pick user");
            for (int i = 1; i < user_count; i++) {   // start at 1: skip Admin
                snprintf(line, sizeof(line), "%d %-8s %-3s %s", users[i].id, users[i].name,   // %-8s = left-align
                         users[i].active ? "ON" : "OFF", users[i].card.value != 0 ? "card" : "-");
                print_at(0, 14 + i * 10, 1, line);
            }
            break;

        case ST_ADMIN_CARD:
            print_at(0, 14, 1, "Tap new card for");
            print_at(0, 28, 2, admin_target->name);
            print_at(0, 54, 1, "*=Cancel");
            break;

        default:                              // any other value: draw nothing
            break;
    }
    display.display();                        // send drawing to the screen
}


/* 13. CLOUD: Wi-Fi, Firebase, phone alerts
 * Firebase paths: /logs (every log entry), /status (current state)
 * HTTPClient and WiFiClientSecure are library objects that the code only calls.
 */

// Adds a message to the circular alert queue (dropped if full)
void queue_alert(const char *text)
{
    if (alert_count >= ALERT_QUEUE_SIZE) return;                // queue full
    int slot = (alert_head + alert_count) % ALERT_QUEUE_SIZE;   // wrap around
    copy_text(alert_queue[slot], text, ALERT_LENGTH);           // store the text
    alert_count++;
}

void connect_wifi(void)
{
    Serial.print("Connecting to Wi-Fi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);      // channel 6 = faster in Wokwi

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {   // try up to 10 s
        delay(250);
        Serial.print(".");
    }
    wifi_ok = (WiFi.status() == WL_CONNECTED);
    Serial.println(wifi_ok ? " connected" : " failed - running offline");
    if (!wifi_ok) return;                         // no Wi-Fi: locker still works

    configTime(0, 0, "pool.ntp.org");             // real date and time (NTP)
    start = millis();
    while (time(NULL) < 1000000000 && millis() - start < 5000) delay(200);   // wait up to 5 s for the time

    secure_client.setInsecure();   // skips certificate check: simulation only
    http.setReuse(true);           // keep the connection open = faster
}

/* Sends one request to Firebase; returns the HTTP code (200 = OK).
 * "POST" adds a new entry, "PUT" replaces the data at that path. */
int firebase_request(const char *method, const char *path, const char *json)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s.json", FIREBASE_URL, path);   // e.g. .../logs.json

    http.begin(secure_client, url);
    http.addHeader("Content-Type", "application/json");   // "we are sending JSON"
    int code = http.sendRequest(method, json);
    http.end();
    return code;
}

/* Sends the oldest alert to the ntfy app; it stays queued if sending fails.
 * Returns true if there was an alert to send. */
bool send_next_alert(void)
{
    if (alert_count == 0) return false;           // nothing to send

    char url[96];
    snprintf(url, sizeof(url), "https://ntfy.sh/%s", NTFY_TOPIC);

    http.begin(secure_client, url);
    http.addHeader("Title", "Smart Locker alert");   // notification title
    http.addHeader("Priority", "urgent");
    http.addHeader("Tags", "rotating_light");     // siren icon
    int code = http.sendRequest("POST", alert_queue[alert_head]);   // send the oldest
    http.end();

    if (code == 200) {                            // sent: remove it
        alert_head = (alert_head + 1) % ALERT_QUEUE_SIZE;   // move head, wrap around
        alert_count--;
    }
    return true;
}

/* Uploads the oldest log entry not sent yet (offline entries are sent later).
 * Returns true if there was something to send. */
bool upload_next_log(void)
{
    LogEntry *e = log_head;
    while (e != NULL && e->uploaded) e = e->next;     // walk the linked list
    if (e == NULL) return false;                      // all uploaded

    char json[160];
    snprintf(json, sizeof(json), "{\"n\":%lu,\"t\":%ld,\"event\":\"%s\",\"user\":\"%s\"}",   // \" = quote inside a string
             e->number, (long)e->timestamp, event_names[e->type], e->user_name);

    if (firebase_request("POST", "/logs", json) == 200) e->uploaded = true;   // mark as sent
    return true;
}

// Shares the current state so the dashboard can show it
void send_status(void)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"locked\":%s,\"alarm\":%s,\"door\":\"%s\",\"t\":%ld}",
             locked ? "true" : "false", alarm_active ? "true" : "false",
             door_closed ? "closed" : "open", (long)time(NULL));
    firebase_request("PUT", "/status", json);     // PUT replaces the old status
}

// Does ONE cloud job every 2 seconds: alerts first, then logs, then status
void network_update(unsigned long now)
{
    static unsigned long last_run = 0;        // static local

    if (!wifi_ok || WiFi.status() != WL_CONNECTED || now - last_run < NET_INTERVAL_MS) return;   // offline or too soon

    /* a cloud request pauses the program briefly, so skip it
     * while someone is typing, to avoid missing key presses */
    if (state == ST_WAIT_PIN || state >= ST_ADMIN_MENU) return;

    last_run = now;
    if (send_next_alert()) return;            // 1st: urgent alerts
    if (upload_next_log()) return;            // 2nd: waiting logs
    send_status();                            // 3rd: live status
}


// 14. SETUP AND LOOP: setup() runs once, loop() runs forever
void setup()
{
    Serial.begin(115200);                     // start Serial Monitor at 115200 baud

    pinMode(PIN_LED_GREEN, OUTPUT);           // pins we drive
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_DOOR, INPUT);                 // pin we read

    lock_servo.setPeriodHertz(50);            // servos use a 50 Hz signal
    lock_servo.attach(PIN_SERVO, 500, 2400);  // pulse range 500-2400 microseconds
    set_lock(true);                           // start locked

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED not found");   // 0x3C = OLED I2C address

    SPI.begin();                              // start SPI for RFID
    rfid.PCD_Init();                          // start the RFID reader

    file_ok = LittleFS.begin(true);    // true = format if needed
    DEBUG_PRINT(file_ok ? "File system ready" : "File system not available");

    setup_users();                            // add the 5 users
    connect_wifi();                           // Wi-Fi + real time
    door_closed = (digitalRead(PIN_DOOR) == DOOR_CLOSED_LEVEL);   // door state at start
    set_state(ST_IDLE);
    log_event(EV_BOOT, NULL);                 // first log entry

    Serial.println("System Ready");
    Serial.println("Cards: Key Fob=Admin/0000  Blue=Alice/1234  Green=Bob/5678");
    Serial.println("       Yellow=Charlie/2468  Red=unknown");
}

void loop()
{
    static unsigned long last_draw = 0;    // static local

    // 1. collect this moment's inputs into one Event
    Event ev;
    ev.now = millis();
    ev.key = keypad.getKey();              // 0 if no key
    ev.card_tapped = read_card(&ev.card);  // pass the address
    if (ev.key != 0) beep_click();

    // 2. the door is checked in every state
    check_door(ev.now);

    // 3. call the current state's function through the table
    state_table[state](&ev);

    // 4. update outputs
    update_leds_and_alarm(ev.now);
    if (ev.now - last_draw >= 150) {       // redraw OLED every 150 ms, not every loop
        draw_screen(ev.now);
        last_draw = ev.now;
    }

    // 5. cloud: alerts, logs and status
    network_update(ev.now);
}