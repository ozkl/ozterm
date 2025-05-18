# Ozterm
One header/source file library to create a terminal app.

It handles all escape sequences. You only implement your window events and drawings.

## Steps
- Create a pseudo terminal (for example using forkpty()).
- When you read from master side, call ozterm_have_read_from_master().
- When key press events are received from your window, call ozterm_send_key().
- Render the buffer in Ozterm->screen_active->buffer

That's all. Now you have a working terminal without any dependency.

main.c implements a sample terminal using SDL library.
