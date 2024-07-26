#include "scene_game.h"
#include "app.h"
#include "save_data_manager.h"
#include "wave/scene_management.h"
#include "wave/calc.h"
#include "wave/data_structures/stack.h"
#include "wave/data_structures/list.h"
#include "wave/data_structures/string_writer.h"
#include "wave/files/file_lines_reader.h"
#include "wave/exception_manager.h"
#include "racso_sokoban_icons.h"
#include <dolphin/dolphin.h>
#include <gui/gui.h>
#include <furi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <storage/storage.h>

#define MAX_READ_BUFFER_SIZE 256
#define MAX_FILENAME_LEN 256
#define MAX_BOARD_SIZE 50
#define MAX_UNDO_STATES 10

typedef enum
{
    CellType_Empty,
    CellType_Wall,
    CellType_Box,
    CellType_Target,
    CellType_Player,
    CellType_BoxOnTarget,
    CellType_PlayerOnTarget
} CellType;

typedef struct Level
{
    int level_width, level_height;
    int cell_size;
    CellType board[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
} Level;

typedef struct GameState
{
    int playerX, playerY, pushesCount;
    CellType board[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
} GameState;

typedef struct GameContext
{
    Stack* states;
    Level* level;
    bool isCompleted;
} GameContext;

static GameContext game;

// Victory Popup component
void victory_popup_render_callback(Canvas* const canvas, AppContext* app)
{
    const int ICON_SIDE = 9;

    AppGameplayState* gameplayState = app->gameplay;
    LevelsDatabase* database = app->database;
    LevelItem* levelItem = &database->collections[gameplayState->selectedCollection].levels[gameplayState->selectedLevel];

    GameState* state = stack_peek(game.states);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "COMPLETED!");

    canvas_set_font(canvas, FontSecondary);

    StringWriter* writer = string_writer_alloc(100);
    string_writer_add_str(writer, "Pushes: ");
    string_writer_add_int(writer, state->pushesCount);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, string_writer_get(writer));

    canvas_draw_icon(canvas, 32 - ICON_SIDE / 2, 28, &I_checkbox_checked);
    string_writer_clear(writer);
    string_writer_add_str(writer, "Best: ");
    string_writer_add_int(writer, levelItem->playerBest);
    canvas_draw_str_aligned(canvas, 32, 42, AlignCenter, AlignCenter, string_writer_get(writer));

    canvas_draw_icon(canvas, 96 - ICON_SIDE / 2, 28, &I_star);
    string_writer_clear(writer);
    string_writer_add_str(writer, "World: ");
    string_writer_add_int(writer, levelItem->worldBest);
    canvas_draw_str_aligned(canvas, 96, 42, AlignCenter, AlignCenter, string_writer_get(writer));

    string_writer_free(writer);

    const int START_CENTER_X = 100, START_CENTER_Y = 59;
    canvas_draw_circle(canvas, START_CENTER_X, START_CENTER_Y, 4);
    canvas_draw_disc(canvas, START_CENTER_X, START_CENTER_Y, 2);
    canvas_draw_str_aligned(canvas, START_CENTER_X + 8, START_CENTER_Y + 4, AlignLeft, AlignBottom, "Next");
}

void victory_popup_handle_input(InputKey key, InputType type, AppContext* app)
{
    AppGameplayState* gameplayState = app->gameplay;

    if (key == InputKeyOk && type == InputTypePress)
    {
        if (gameplayState->selectedLevel + 1 < app->database->collections[gameplayState->selectedCollection].levelsCount)
        {
            gameplayState->selectedLevel += 1;
            scene_manager_set_scene(app->sceneManager, SceneType_Game);
            return;
        }
        else
        {
            scene_manager_set_scene(app->sceneManager, SceneType_Menu);
            return;
        }
    }
}

static int level_reader_parse_row(const char* line, CellType* row)
{
    const char *ch = line;
    int i = 0;
    while (true)
    {
        if (i >= MAX_BOARD_SIZE)
            return -1;
        switch (*(ch++))
        {
        case '\0':
            return i;
        case '#':
            row[i++] = CellType_Wall;
            break;
        case '*':
            row[i++] = CellType_BoxOnTarget;
            break;
        case '.':
            row[i++] = CellType_Target;
            break;
        case '@':
            row[i++] = CellType_Player;
            break;
        case '+':
            row[i++] = CellType_PlayerOnTarget;
            break;
        case '$':
            row[i++] = CellType_Box;
            break;
        case ' ':
            row[i++] = CellType_Empty;
            break;
        default:
            return -2;
        }
    }
}

static int level_reader_calculate_cell_size(int width, int height)
{
    #define MAX_WIDTH (128 + (cellSize - 1) * 2)
    #define MAX_HEIGHT (64 + (cellSize - 1) * 2)

    int cellSize = 9;
    if (width * cellSize > MAX_WIDTH || height * cellSize > MAX_HEIGHT)
        cellSize = 7;
    if (width * cellSize > MAX_WIDTH || height * cellSize > MAX_HEIGHT)
        cellSize = 5;
    return cellSize;

    #undef MAX_WIDTH
    #undef MAX_HEIGHT
}

void level_reader_load_level(Level* ret_level, FileLinesReader* reader, int levelIndex)
{
    CellType board[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {CellType_Empty};
    int columnCount = 0, rowCount = 0;
    char line[MAX_READ_BUFFER_SIZE];

    char levelStartMark[16];
    snprintf(levelStartMark, sizeof(levelStartMark), "%d", levelIndex + 1);

    bool levelFound = false;
    while (!levelFound && file_lines_reader_readln(reader, line, sizeof(line)))
        if (strncmp(levelStartMark, line, sizeof(levelStartMark)) == 0)
            levelFound = true;

    furi_check(levelFound, "level not found");

    while (file_lines_reader_readln(reader, line, sizeof(line)))
    {
        int rowSize = level_reader_parse_row(line, board[rowCount]);
        if (rowSize < 0)
            break;
        if (rowSize > columnCount)
            columnCount = rowSize;
        rowCount += 1;
        if (rowCount >= MAX_BOARD_SIZE)
            break;
    }

    int naturalCellSize = level_reader_calculate_cell_size(columnCount, rowCount);
    int rotatedCellSize = level_reader_calculate_cell_size(rowCount, columnCount);
    if (naturalCellSize >= rotatedCellSize)
    {
        ret_level->cell_size = naturalCellSize;
        ret_level->level_width = columnCount;
        ret_level->level_height = rowCount;
        memcpy(ret_level->board, board, sizeof(board));
    }
    else
    {
        ret_level->cell_size = rotatedCellSize;
        ret_level->level_width = rowCount;
        ret_level->level_height = columnCount;
        for (int row = 0; row < rowCount; row++)
            for (int column = 0; column < columnCount; column++)
                ret_level->board[column][row] = board[row][column];
    }
}

const Icon* findIcon(CellType cellType, int size)
{
    switch (cellType)
    {
    case CellType_Wall:
        switch (size)
        {
        case 5:
            return &I_cell_wall_5;
        case 7:
            return &I_cell_wall_7;
        case 9:
            return &I_cell_wall_9;
        }
        break;

    case CellType_Box:
        switch (size)
        {
        case 5:
            return &I_cell_box_5;
        case 7:
            return &I_cell_box_7;
        case 9:
            return &I_cell_box_9;
        }
        break;

    case CellType_Target:
        switch (size)
        {
        case 5:
            return &I_cell_target_5;
        case 7:
            return &I_cell_target_7;
        case 9:
            return &I_cell_target_9;
        }
        break;

    case CellType_Player:
        switch (size)
        {
        case 5:
            return &I_cell_player_5;
        case 7:
            return &I_cell_player_7;
        case 9:
            return &I_cell_player_9;
        }
        break;

    case CellType_BoxOnTarget:
        switch (size)
        {
        case 5:
            return &I_cell_box_target_5;
        case 7:
            return &I_cell_box_target_7;
        case 9:
            return &I_cell_box_target_9;
        }
        break;

    case CellType_PlayerOnTarget:
        switch (size)
        {
        case 5:
            return &I_cell_player_target_5;
        case 7:
            return &I_cell_player_target_7;
        case 9:
            return &I_cell_player_target_9;
        }
        break;

    default:
        return NULL;
    }

    return NULL;
}

void draw_game(Canvas* const canvas, GameContext* game)
{
    GameState* state = stack_peek(game->states);
    Level *level = game->level;

    int cellSize = level->cell_size;
    int levelWidth = level->level_width * cellSize;
    int levelHeight = level->level_height * cellSize;

    int playerX = state->playerX * cellSize;
    int playerY = state->playerY * cellSize;

    int screenWidth = 128;
    int screenHeight = 64;

    int minScrollingWidth = screenWidth + (cellSize - 1) * 2;
    int minScrollingHeight = screenHeight + (cellSize - 1) * 2;

    int cameraX = levelWidth / 2;
    if (levelWidth > minScrollingWidth)
        cameraX = MAX(screenWidth / 2, MIN(playerX, levelWidth - screenWidth / 2));

    int cameraY = levelHeight / 2;
    if (levelHeight > minScrollingHeight)
        cameraY = MAX(screenHeight / 2, MIN(playerY, levelHeight - screenHeight / 2));

    for (int row = 0; row < level->level_height; row++)
    {
        for (int column = 0; column < level->level_width; column++)
        {
            int x = column * cellSize - cameraX + screenWidth / 2;
            int y = row * cellSize - cameraY + screenHeight / 2;
            const Icon* icon = findIcon(state->board[row][column], cellSize);
            if (icon)
                canvas_draw_icon(canvas, x, y, icon);
        }
    }
}

void game_render_callback(Canvas* const canvas, void* context)
{
    AppContext* app = (AppContext*)context;

    canvas_clear(canvas);
    GameState* state = stack_peek(game.states);
    Level* level = game.level;

    if (state == NULL || level == NULL)
        return;

    draw_game(canvas, &game);

    if (game.isCompleted)
        victory_popup_render_callback(canvas, app);
}

void game_state_initialize(GameState* state, Level* level)
{
    if (state == NULL)
    {
        throw_exception("state cannot be null");
        return;
    }

    if (level == NULL)
    {
        throw_exception("level cannot be null");
        return;
    }

    state->playerX = state->playerY = state->pushesCount = 0;
    memcpy(state->board, level->board, sizeof(CellType) * MAX_BOARD_SIZE * MAX_BOARD_SIZE);

    for (int y = 0; y < level->level_height; y++)
    {
        for (int x = 0; x < level->level_width; x++)
        {
            if (state->board[y][x] == CellType_Player || state->board[y][x] == CellType_PlayerOnTarget)
            {
                state->playerX = x;
                state->playerY = y;
                return;
            }
        }
    }
}

void load_selected_level(AppGameplayState* gameplayState, LevelsDatabase* database)
{
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    char filename[MAX_FILENAME_LEN];
    snprintf(filename, MAX_FILENAME_LEN, "%s/%s.txt", STORAGE_APP_ASSETS_PATH_PREFIX, database->collections[gameplayState->selectedCollection].name);
    for (int i = 0; filename[i] != '\0'; i++)
        if (filename[i] >= 'A' && filename[i] <= 'Z')
            filename[i] += 'a' - 'A';

    FURI_LOG_D("GAME", "Opening file: %s", filename);
    storage_file_open(file, filename, FSAM_READ, FSOM_OPEN_EXISTING);
    FileLinesReader* reader = file_lines_reader_alloc(file, MAX_READ_BUFFER_SIZE);

    FURI_LOG_D("GAME", "Loading level %d", gameplayState->selectedLevel);
    level_reader_load_level(game.level, reader, gameplayState->selectedLevel);
    FURI_LOG_D("GAME", "Level size: %d x %d", game.level->level_width, game.level->level_height);

    GameState* initialState = malloc(sizeof(GameState));
    game_state_initialize(initialState, game.level);
    stack_push(game.states, initialState);

    file_lines_reader_free(reader);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void game_transition_callback(int from, int to, void* context)
{
    AppContext* app = (AppContext*)context;
    AppGameplayState* gameplayState = app->gameplay;
    LevelsDatabase* database = app->database;

    if (from == SceneType_Game)
    {
        while (stack_count(game.states) > 0)
        {
            GameState* state = stack_pop(game.states);
            free(state);
        }

        stack_free(game.states);
        free(game.level);
    }

    if (to == SceneType_Game)
    {
        game.level = malloc(sizeof(Level));
        game.states = stack_alloc();
        game.isCompleted = false;
        load_selected_level(gameplayState, database);
    }
}

void verify_level_completed(GameState* state)
{
    for (int y = 0; y < MAX_BOARD_SIZE; y++)
    {
        for (int x = 0; x < MAX_BOARD_SIZE; x++)
        {
            if (state->board[y][x] == CellType_Box)
                return;
        }
    }

    game.isCompleted = true;
}

void apply_input(GameState* gameState, int dx, int dy, Level* level)
{
    int newX = gameState->playerX + dx, newY = gameState->playerY + dy;
    if (newX < 0 || newX >= level->level_width || newY < 0 || newY >= level->level_height)
        return;

    CellType newCell = gameState->board[newY][newX];
    if (newCell == CellType_Wall)
        return;

    if (newCell == CellType_Box || newCell == CellType_BoxOnTarget)
    {
        int newBoxX = newX + dx, newBoxY = newY + dy;
        if (newBoxX < 0 || newBoxX >= level->level_width || newBoxY < 0 || newBoxY >= level->level_height)
            return;

        CellType newBoxCell = gameState->board[newBoxY][newBoxX];
        if (newBoxCell == CellType_Wall || newBoxCell == CellType_Box || newBoxCell == CellType_BoxOnTarget)
            return;

        gameState->board[newBoxY][newBoxX] = newBoxCell == CellType_Target ? CellType_BoxOnTarget : CellType_Box;
        newCell = newCell == CellType_BoxOnTarget ? CellType_Target : CellType_Empty;
        gameState->board[newY][newX] = newCell;
        gameState->pushesCount += 1;
    }

    gameState->board[newY][newX] = newCell == CellType_Target ? CellType_PlayerOnTarget : CellType_Player;
    gameState->board[gameState->playerY][gameState->playerX] = gameState->board[gameState->playerY][gameState->playerX] == CellType_PlayerOnTarget ? CellType_Target : CellType_Empty;
    gameState->playerX = newX;
    gameState->playerY = newY;
}

void game_handle_player_input(InputKey key, InputType type, GameContext* gameContext)
{
    if (type != InputTypePress && type != InputTypeRepeat)
        return;

    int dx = 0, dy = 0;
    switch (key)
    {
    case InputKeyLeft:
        dx = -1;
        break;
    case InputKeyRight:
        dx = 1;
        break;
    case InputKeyUp:
        dy = -1;
        break;
    case InputKeyDown:
        dy = 1;
        break;
    default:
        return;
    }

    Level* level = gameContext->level;
    GameState* previousGameState = stack_peek(gameContext->states);
    GameState* gameState = malloc(sizeof(GameState));
    memcpy(gameState, previousGameState, sizeof(GameState));

    apply_input(gameState, dx, dy, level);

    if (gameState->playerX != previousGameState->playerX || gameState->playerY != previousGameState->playerY)
    {
        if (stack_count(gameContext->states) >= MAX_UNDO_STATES)
        {
            GameState* state = stack_discard_bottom(gameContext->states);
            free(state);
        }

        stack_push(gameContext->states, gameState);
        verify_level_completed(gameState);
    }
    else
    {
        free(gameState);
    }
}

void game_handle_input(InputKey key, InputType type, void* context)
{
    AppContext* app = (AppContext*)context;
    GameState* gameState = stack_peek(game.states);

    if (key == InputKeyBack && type == InputTypePress)
    {
        scene_manager_set_scene(app->sceneManager, SceneType_Menu);
        return;
    }

    if (game.isCompleted)
    {
        victory_popup_handle_input(key, type, app);
        return;
    }

    if (key == InputKeyOk && (type == InputTypePress || type == InputTypeRepeat))
    {
        if (stack_count(game.states) > 1)
        {
            GameState* state = stack_pop(game.states);
            free(state);
        }
        return;
    }

    game_handle_player_input(key, type, &game);
    gameState = stack_peek(game.states);

    if (game.isCompleted)
    {
        FURI_LOG_D("GAME", "Level completed in %d pushes", gameState->pushesCount);

        dolphin_deed(DolphinDeedPluginGameWin);
        AppGameplayState* gameplayState = app->gameplay;
        LevelsDatabase* database = app->database;

        LevelItem* levelItem = &database->collections[gameplayState->selectedCollection].levels[gameplayState->selectedLevel];
        if (levelItem->playerBest == 0 || gameState->pushesCount < levelItem->playerBest)
        {
            levelItem->playerBest = gameState->pushesCount;
            levels_database_save_player_progress(database);
        }
    }
}

void game_tick_callback(void* context)
{
    AppContext* app = (AppContext*)context;
    UNUSED(app);
}