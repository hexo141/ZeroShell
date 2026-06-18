#include "liquid_module.h"

#include <iostream>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <windows.h>
#include <conio.h>

// ============================================================
//  ASCII Liquid Simulation
//  - Particles with position/velocity affected by gravity
//  - Collision with rectangular boundary (bounce + damping)
//  - Particle-particle collision for fluid-like behavior
//  - Rendered as ASCII density map inside a bordered rectangle
// ============================================================

struct Particle {
    float x, y;     // position (in grid units)
    float vx, vy;   // velocity
};

std::vector<std::string> LiquidModule::getCommands() const {
    return { "liquid" };
}

bool LiquidModule::execute(const std::string& cmd, const std::vector<std::string>& /*args*/) {
    if (cmd == "liquid") {
        runSimulation();
        return true;
    }
    return false;
}

void LiquidModule::runSimulation() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldInMode, oldOutMode;
    GetConsoleMode(hIn, &oldInMode);
    GetConsoleMode(hOut, &oldOutMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hOut, oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);

    // Simulation area (inside border)
    int simW = 50;
    int simH = 25;
    int consoleW = csbi.dwSize.X;
    if (simW + 4 > consoleW) simW = consoleW - 4;
    if (simW < 20) simW = 20;
    if (simH < 15) simH = 15;

    // Particle count
    const int NUM_PARTICLES = 350;

    // Physics constants
    const float gravity = 22.0f;       // units/s^2
    const float bounce = 0.35f;        // energy retention on wall bounce
    const float friction = 0.995f;     // velocity damping per frame
    const float particleR = 0.45f;     // particle radius for collision (smaller = can pack tighter)
    const float repulsion = 120.0f;    // repulsion force between particles
    const float cohesion = 3.0f;       // cohesion: attract particles that are close but not too close
    const float dt = 0.033f;           // ~30 fps timestep

    // Initialize particles - drop from top
    std::vector<Particle> particles(NUM_PARTICLES);
    for (int i = 0; i < NUM_PARTICLES; ++i) {
        particles[i].x = 2.0f + (float)(rand() % (simW - 4));
        particles[i].y = 1.0f + (float)(rand() % (simH / 3));
        particles[i].vx = (float)(rand() % 20 - 10) * 0.3f;
        particles[i].vy = (float)(rand() % 5) * 0.3f;
    }

    // Density grid for rendering
    std::vector<std::vector<float>> density(simH, std::vector<float>(simW, 0.0f));

    // ASCII density characters (sparse to dense)
    const char* densityChars = " .:-=+*#%@";
    const int numLevels = 10;

    // Color palette for density levels (blue shades)
    const char* densityColors[] = {
        "",                          // 0: empty
        "\x1b[38;2;20;40;80m",      // 1
        "\x1b[38;2;30;60;120m",     // 2
        "\x1b[38;2;40;90;160m",     // 3
        "\x1b[38;2;50;120;200m",    // 4
        "\x1b[38;2;60;150;230m",    // 5
        "\x1b[38;2;80;180;255m",    // 6
        "\x1b[38;2;120;200;255m",   // 7
        "\x1b[38;2;180;230;255m",   // 8
        "\x1b[38;2;220;245;255m",   // 9
    };

    SHORT startY = csbi.dwCursorPosition.Y;
    DWORD written;

    // Build border characters
    auto drawFrame = [&]() {
        // Top border
        COORD pos = { 0, startY };
        SetConsoleCursorPosition(hOut, pos);
        std::string top = "\x1b[38;2;100;100;120m+";
        for (int x = 0; x < simW; ++x) top += "-";
        top += "+\x1b[0m\x1b[K";
        WriteConsoleA(hOut, top.c_str(), static_cast<DWORD>(top.size()), &written, nullptr);

        // Side borders + content
        for (int y = 0; y < simH; ++y) {
            COORD rowPos = { 0, static_cast<SHORT>(startY + 1 + y) };
            SetConsoleCursorPosition(hOut, rowPos);

            std::string left = "\x1b[38;2;100;100;120m|\x1b[0m";
            WriteConsoleA(hOut, left.c_str(), static_cast<DWORD>(left.size()), &written, nullptr);

            std::string row;
            for (int x = 0; x < simW; ++x) {
                int d = (int)(density[y][x] * (numLevels - 1));
                if (d < 0) d = 0;
                if (d >= numLevels) d = numLevels - 1;
                if (d == 0) {
                    row += ' ';
                } else {
                    row += densityColors[d];
                    row += densityChars[d];
                }
            }
            row += "\x1b[0m";
            WriteConsoleA(hOut, row.c_str(), static_cast<DWORD>(row.size()), &written, nullptr);

            std::string right = "\x1b[38;2;100;100;120m|\x1b[0m\x1b[K";
            WriteConsoleA(hOut, right.c_str(), static_cast<DWORD>(right.size()), &written, nullptr);
        }

        // Bottom border
        COORD botPos = { 0, static_cast<SHORT>(startY + 1 + simH) };
        SetConsoleCursorPosition(hOut, botPos);
        std::string bot = "\x1b[38;2;100;100;120m+";
        for (int x = 0; x < simW; ++x) bot += "-";
        bot += "+\x1b[0m\x1b[K";
        WriteConsoleA(hOut, bot.c_str(), static_cast<DWORD>(bot.size()), &written, nullptr);

        // Info line
        COORD infoPos = { 0, static_cast<SHORT>(startY + 2 + simH) };
        SetConsoleCursorPosition(hOut, infoPos);
        std::string info = "\x1b[38;2;128;128;128m  [Space] splash  [R] reset  [Esc] quit\x1b[0m\x1b[K";
        WriteConsoleA(hOut, info.c_str(), static_cast<DWORD>(info.size()), &written, nullptr);
    };

    // Hide cursor
    std::string hideCur = "\x1b[?25l";
    WriteConsoleA(hOut, hideCur.c_str(), static_cast<DWORD>(hideCur.size()), &written, nullptr);

    bool running = true;
    while (running) {
        // Check for input (non-blocking)
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 27) { // ESC
                running = false;
                break;
            } else if (ch == ' ' || ch == 's') {
                // Splash: apply random upward force to all particles
                for (auto& p : particles) {
                    p.vy -= 15.0f + (float)(rand() % 20);
                    p.vx += (float)(rand() % 30 - 15);
                }
            } else if (ch == 'r' || ch == 'R') {
                // Reset
                for (int i = 0; i < NUM_PARTICLES; ++i) {
                    particles[i].x = 2.0f + (float)(rand() % (simW - 4));
                    particles[i].y = 1.0f + (float)(rand() % (simH / 3));
                    particles[i].vx = (float)(rand() % 40 - 20) * 0.5f;
                    particles[i].vy = (float)(rand() % 10) * 0.5f;
                }
            }
        }

        // --- Physics step ---

        // Apply gravity and friction
        for (auto& p : particles) {
            p.vy += gravity * dt;
            p.vx *= friction;
            p.vy *= friction;
        }

        // Particle-particle repulsion (spatial hash for performance)
        // Simple grid-based neighbor search
        int cellSize = 3;
        int gridW = simW / cellSize + 1;
        int gridH = simH / cellSize + 1;
        std::vector<std::vector<int>> grid(gridW * gridH);

        for (int i = 0; i < NUM_PARTICLES; ++i) {
            int cx = (int)particles[i].x / cellSize;
            int cy = (int)particles[i].y / cellSize;
            if (cx < 0) cx = 0; if (cx >= gridW) cx = gridW - 1;
            if (cy < 0) cy = 0; if (cy >= gridH) cy = gridH - 1;
            grid[cy * gridW + cx].push_back(i);
        }

        for (int ci = 0; ci < gridW * gridH; ++ci) {
            int cx = ci % gridW;
            int cy = ci / gridW;
            // Check this cell and neighbors
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = cx + dx;
                    int ny = cy + dy;
                    if (nx < 0 || nx >= gridW || ny < 0 || ny >= gridH) continue;
                    int ni = ny * gridW + nx;
                    for (int a : grid[ci]) {
                        for (int b : grid[ni]) {
                            if (a >= b) continue;
                            float ddx = particles[a].x - particles[b].x;
                            float ddy = particles[a].y - particles[b].y;
                            float dist2 = ddx * ddx + ddy * ddy;
                            float minDist = particleR * 2.2f;
                            float attractDist = particleR * 4.5f;
                            if (dist2 < 0.01f) continue;
                            float dist = sqrtf(dist2);

                            if (dist < minDist) {
                                // Repulsion: too close, push apart
                                float overlap = minDist - dist;
                                float nx2 = ddx / dist;
                                float ny2 = ddy / dist;
                                float force = repulsion * overlap * dt;
                                particles[a].vx += nx2 * force;
                                particles[a].vy += ny2 * force;
                                particles[b].vx -= nx2 * force;
                                particles[b].vy -= ny2 * force;
                            } else if (dist < attractDist) {
                                // Cohesion: attract nearby particles for liquid-like behavior
                                float nx2 = ddx / dist;
                                float ny2 = ddy / dist;
                                float t = (dist - minDist) / (attractDist - minDist);
                                float force = cohesion * (1.0f - t) * dt;
                                particles[a].vx -= nx2 * force;
                                particles[a].vy -= ny2 * force;
                                particles[b].vx += nx2 * force;
                                particles[b].vy += ny2 * force;
                            }
                        }
                    }
                }
            }
        }

        // Update positions and handle boundary collisions
        for (auto& p : particles) {
            p.x += p.vx * dt;
            p.y += p.vy * dt;

            // Left wall
            if (p.x < 0.5f) {
                p.x = 0.5f;
                p.vx = fabsf(p.vx) * bounce;
            }
            // Right wall
            if (p.x > simW - 1.5f) {
                p.x = simW - 1.5f;
                p.vx = -fabsf(p.vx) * bounce;
            }
            // Top wall
            if (p.y < 0.5f) {
                p.y = 0.5f;
                p.vy = fabsf(p.vy) * bounce;
            }
            // Bottom wall (floor)
            if (p.y > simH - 1.5f) {
                p.y = simH - 1.5f;
                p.vy = -fabsf(p.vy) * bounce;
                // Extra floor friction
                p.vx *= 0.95f;
            }
        }

        // --- Render ---

        // Clear density grid
        for (int y = 0; y < simH; ++y)
            for (int x = 0; x < simW; ++x)
                density[y][x] = 0.0f;

        // Splat particles onto density grid (gaussian-like)
        for (const auto& p : particles) {
            int ix = (int)p.x;
            int iy = (int)p.y;
            // Splat a 5x5 area for denser look
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    int tx = ix + dx;
                    int ty = iy + dy;
                    if (tx < 0 || tx >= simW || ty < 0 || ty >= simH) continue;
                    float ddx = p.x - tx;
                    float ddy = p.y - ty;
                    float d2 = ddx * ddx + ddy * ddy;
                    float w = expf(-d2 * 1.2f);
                    density[ty][tx] += w * 0.8f;
                }
            }
        }

        // Clamp density
        for (int y = 0; y < simH; ++y)
            for (int x = 0; x < simW; ++x)
                if (density[y][x] > 1.0f) density[y][x] = 1.0f;

        drawFrame();

        Sleep(33); // ~30 fps
    }

    // Cleanup: clear simulation area
    for (int i = 0; i < simH + 3; ++i) {
        COORD pos = { 0, static_cast<SHORT>(startY + i) };
        SetConsoleCursorPosition(hOut, pos);
        std::string cl = "\x1b[K";
        WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
    }
    COORD endPos = { 0, startY };
    SetConsoleCursorPosition(hOut, endPos);

    // Show cursor
    std::string showCur = "\x1b[?25h";
    WriteConsoleA(hOut, showCur.c_str(), static_cast<DWORD>(showCur.size()), &written, nullptr);

    SetConsoleMode(hIn, oldInMode);
    SetConsoleMode(hOut, oldOutMode);
}
