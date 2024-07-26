#pragma once

#define MAX_BOARD_SIZE 50

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

void level_load(Level* ret_level, const char* collectionName, int levelIndex);