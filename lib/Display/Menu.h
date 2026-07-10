#ifndef MENU_H
#define MENU_H

#include <Arduino.h>
#include <functional>

// Menu item callback function type
typedef std::function<void(void)> MenuCallback;

// Menu item structure
struct MenuItem {
    const char* label;
    MenuCallback callback;
};

// Menu state structure
struct Menu {
    const char* title;
    const MenuItem* items;
    uint8_t itemCount;
    uint8_t selectedIndex;  // Currently selected menu item
    
    Menu(const char* t, const MenuItem* i, uint8_t c) 
        : title(t), items(i), itemCount(c), selectedIndex(0) {}
};

#endif // MENU_H
