ADVnoid, a custom brick breaker built from scratch for the Cardputer ADV and developed with PlatformIO and ChatGPT 5.4 Codex.

It was designed first for use with the GameCase, but controls are fully rebindable, so it can be adapted to different setups.

Update: version 0.2 out now with an Easter egg encoded thx to u/MrAjAnderson! To enable it, just remember those old days with Konami games and easy cheats using only some buttons....

Main features
Arcade-inspired brick breaker gameplay
Custom title screen and animated UI
Multiple brick types, including tougher bricks with consistent hit-color progression
Indestructible walls and moving wall elements in later stages

Power-ups including:
paddle expand
laser
slow ball
multiball

Particle explosion effect on destructible brick hits

Selectable gameplay backgrounds (twelve)

Random or custom background mode

High score saving on SD

Name entry screen for new high scores

MP3 playback from SD with playlist support

Separate SFX and music volume controls

Pause, help, options, and background selector screens

Controls
Default controls are:
A = move left
D = move right
P = launch ball / fire laser
B = pause
G0 = options or return to title depending on screen
V = help, and during gameplay it opens background select when background mode is set to CUSTOM
<- and -> arrow = previous / next MP3 track

Options menu
The options menu includes:
SFX on/off
SFX volume
music on/off
music volume
difficulty
paddle size
trails on/off
background mode: RANDOM or CUSTOM
key rebinding for left / right / action
track info
playlist editor

All settings are saved to SD in the /ADVnoid folder.
High scores are also saved there.

MP3 support
ADVnoid supports MP3 playback from SD:
music files go into /ADVnoid/music
playlist is managed in-game
playback continues independently from gameplay
separate music volume from SFX volume

Storage
The game automatically creates:

/ADVnoid
/ADVnoid/music
config file
playlist file

high score file
