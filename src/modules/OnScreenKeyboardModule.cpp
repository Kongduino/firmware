#include "configuration.h"
#if HAS_SCREEN

#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/NotificationRenderer.h"
#include "input/RotaryEncoderInterruptImpl1.h"
#include "input/UpDownInterruptImpl1.h"
#include "modules/OnScreenKeyboardModule.h"
#include <Arduino.h>
#include <algorithm>

namespace graphics
{

OnScreenKeyboardModule &OnScreenKeyboardModule::instance()
{
    static OnScreenKeyboardModule inst;
    return inst;
}

OnScreenKeyboardModule::~OnScreenKeyboardModule() = default;

void OnScreenKeyboardModule::start(const char *header, const char *initialText, uint32_t durationMs,
                                   std::function<void(const std::string &)> cb)
{
    keyboard = std::make_unique<VirtualKeyboard>();
    callback = cb;
    if (header)
        keyboard->setHeader(header);
    if (initialText)
        keyboard->setInputText(initialText);

    // Route VK submission/cancel events back into the module
    keyboard->setCallback([this](const std::string &text) {
        if (text.empty()) {
            this->onCancel();
        } else {
            this->onSubmit(text);
        }
    });

    // Maintain legacy compatibility hooks
    NotificationRenderer::virtualKeyboard = keyboard.get();
    NotificationRenderer::textInputCallback = callback;
}

void OnScreenKeyboardModule::stop(bool callEmptyCallback)
{
    auto cb = callback;
    callback = nullptr;
    keyboard.reset();
    // Keep NotificationRenderer legacy pointers in sync
    NotificationRenderer::virtualKeyboard = nullptr;
    NotificationRenderer::textInputCallback = nullptr;
    if (callEmptyCallback && cb)
        cb("");
}

void OnScreenKeyboardModule::handleInput(const InputEvent &event)
{
    if (!keyboard)
        return;

    if (processVirtualKeyboardInput(event, keyboard.get()))
        return;

    if (event.inputEvent == INPUT_BROKER_CANCEL)
        onCancel();
}

bool OnScreenKeyboardModule::processVirtualKeyboardInput(const InputEvent &event, VirtualKeyboard *targetKeyboard)
{
    if (!targetKeyboard)
        return false;

#if HAS_TOUCHSCREEN
    // A tap or a long press on the screen presses the key under the finger (swipes still move the cursor). Taps outside
    // the keys (the text) do nothing
    if ((event.touchX != 0 || event.touchY != 0) &&
        (event.inputEvent == INPUT_BROKER_USER_PRESS || event.inputEvent == INPUT_BROKER_SELECT)) {
#ifdef TOUCH_TO_SCREEN_X
        // A touch panel mounted in another orientation than the screen is drawn in (the variant says how)
        const int16_t x = TOUCH_TO_SCREEN_X(event.touchX, event.touchY);
        const int16_t y = TOUCH_TO_SCREEN_Y(event.touchX, event.touchY);
#else
        const int16_t x = event.touchX;
        const int16_t y = event.touchY;
#endif
        targetKeyboard->tapAt(x, y, event.inputEvent == INPUT_BROKER_SELECT);
        return true;
    }
#endif

    switch (event.inputEvent) {
    case INPUT_BROKER_UP:
    case INPUT_BROKER_UP_LONG:
        targetKeyboard->moveCursorUp();
        return true;
    case INPUT_BROKER_DOWN:
    case INPUT_BROKER_DOWN_LONG:
        targetKeyboard->moveCursorDown();
        return true;
    case INPUT_BROKER_LEFT:
    case INPUT_BROKER_ALT_PRESS:
        targetKeyboard->moveCursorLeft();
        return true;
    case INPUT_BROKER_RIGHT:
    case INPUT_BROKER_USER_PRESS:
        targetKeyboard->moveCursorRight();
        return true;
    case INPUT_BROKER_SELECT:
        targetKeyboard->handlePress();
        return true;
    case INPUT_BROKER_SELECT_LONG:
        targetKeyboard->handleLongPress();
        return true;
    default:
        return false;
    }
}

bool OnScreenKeyboardModule::draw(OLEDDisplay *display)
{
    if (!keyboard)
        return false;

    // Timeout
    if (keyboard->isTimedOut()) {
        onCancel();
        return false;
    }

    // Clear full screen behind keyboard
    display->setColor(BLACK);
    display->fillRect(0, 0, display->getWidth(), display->getHeight());
    display->setColor(WHITE);
    keyboard->draw(display, 0, 0);
    return true;
}

void OnScreenKeyboardModule::onSubmit(const std::string &text)
{
    auto cb = callback;
    stop(false);
    if (cb)
        cb(text);
}

void OnScreenKeyboardModule::onCancel()
{
    stop(true);
}

} // namespace graphics

#endif // HAS_SCREEN
