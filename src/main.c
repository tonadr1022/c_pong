#include <assert.h>
#include <math.h>
#include <stdio.h>

#define RAYGUI_IMPLEMENTATION
#include <assert.h>
#include <limits.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <unistd.h>

#include "networking.h"
#include "raygui.h"
#include "raylib.h"
#include "raymath.h"

static Vector2 window_dims = {800, 600};
static Vector2 world_dims = {400, 400};
static Vector2 paddle_dims = {10, 80};
static float ball_radius = 8;
static float ball_base_speed_x = 200.f;

typedef enum GameState {
  STATE_MENU,
  STATE_PLAY,
  STATE_PAUSE_MENU,
  STATE_WAIT_FOR_PLAYER_TWO_AS_HOST,
  STATE_COUNT
} GameState;

typedef enum MsgType {
  MSG_PLAYER_POS,
  MSG_SCORE_UPDATE,
  MSG_BALL_POS_UPDATE,
  MSG_STATE_UPDATE,
} MsgType;

typedef struct MsgPlayerPos {
  float pos;
  int player;
} MsgPlayerPos;

typedef struct MsgStateUpdate {
  GameState state;
  int player;
} MsgStateUpdate;

typedef struct MsgScoreUpdate {
  int score;
  int player;
} MsgScoreUpdate;

typedef struct MsgBall {
  Vector2 pos;
  Vector2 velocity;
} MsgBall;

typedef struct MsgPositionUpdate {
  float p;
} MsgPositionUpdate;

typedef struct NetworkMultiplayerData {
  const char* port;
  const char* ip_addr;
  const char* this_machine_ip_addr;
  bool is_host;
  int fd;     // listener fd if host, otherwise fd of the host
  int p2_fd;  // client fd if host, otherwise nothing
  const char* error_msg;
  MsgBuffer msg_buf;
} NetworkMultiplayerData;

typedef struct PlayerData {
  float pos;
  float paddle_vert_velocity;
  int score;
} PlayerData;

typedef struct Game {
  int collision_count;
  Vector2 ball_pos;
  Vector2 ball_velocity;
  Camera2D camera;
  Rectangle viewport;
  float ppu;
  GameState game_state;
  int curr_pause_player;
  PlayerData players[2];
  NetworkMultiplayerData net_info;
} Game;

bool is_online_game(Game* g) { return g->net_info.p2_fd > 0 || g->net_info.fd > 0; }
int get_curr_player(Game* g) { return g->net_info.is_host ? 0 : 1; }

void game_reset_ball(Game* g) {
  int initial_dir = GetRandomValue(0, 1);
  float mult = initial_dir > 0 ? 1 : -1;
  g->ball_velocity = (Vector2){mult * ball_base_speed_x, 0};
  g->ball_pos = (Vector2){world_dims.x / 2.f, world_dims.x / 2.f};
  g->collision_count = 0;
}
int get_other_player_fd(Game* g) {
  return g->net_info.is_host ? g->net_info.p2_fd : g->net_info.fd;
}

void game_start_new_game(Game* g) {
  for (int i = 0; i < 2; i++) {
    PlayerData* p = g->players + i;
    p->pos = world_dims.x / 2.f;
    p->score = 0;
  }
  game_reset_ball(g);
}

void game_init(Game* g) {
  {
    *g = (Game){};
    float scale_x = window_dims.x / world_dims.x;
    float scale_y = window_dims.y / world_dims.y;
    g->ppu = fminf(scale_x, scale_y);
    float view_x = world_dims.x * g->ppu;
    float view_y = world_dims.y * g->ppu;
    g->viewport = (Rectangle){.x = (float)(window_dims.x - view_x) * 0.5f,
                              .y = (float)(window_dims.y - view_y) * 0.5f,
                              .width = view_x,
                              .height = view_y};
    g->camera.zoom = g->ppu;
    g->camera.target = (Vector2){world_dims.x * 0.5f, world_dims.y * 0.5f};
    g->camera.offset = (Vector2){g->viewport.x + g->viewport.width * 0.5f,
                                 g->viewport.y + g->viewport.height * 0.5f};
    msg_buf_init(&g->net_info.msg_buf, 1024);
  }

  g->curr_pause_player = INT_MAX;
  g->game_state = STATE_MENU;
}

void on_local_game_start(Game* g) {
  g->game_state = STATE_PLAY;
  game_start_new_game(g);
}

void set_port(Game* g, int port) {
  char buf[10];
  snprintf(buf, sizeof(buf), "%i", port);
  g->net_info.port = strdup(buf);
}

/**
 *
 * @param net
 * @return success val
 */
bool connect_to_host(NetworkMultiplayerData* net) {
  struct addrinfo* addr_info = get_addr_info(net->port, net->ip_addr);
  int fd = socket(addr_info->ai_family, addr_info->ai_socktype, addr_info->ai_protocol);
  if (fd == -1) {
    perror("socket");
    return false;
  }
  int status;
  if ((status = connect(fd, addr_info->ai_addr, addr_info->ai_addrlen)) == -1) {
    perror("connect");
    close(fd);
    return false;
  }
  net->fd = fd;
  return true;
}

void on_join_online_game(Game* g, int port, const char* ip_addr) {
  printf("joining game on port %i, addr %s\n", port, ip_addr);
  set_port(g, port);
  g->net_info.ip_addr = strdup(ip_addr);
  g->net_info.is_host = false;
  bool success = connect_to_host(&g->net_info);
  if (success) {
    printf("connected to host");
    g->game_state = STATE_PLAY;
  } else {
    g->net_info.error_msg = "Failed to connect to host";
  }
}

void game_update_wait_for_player_2(Game* g) {
  struct pollfd fds[1] = {};
  fds[0].events = POLLIN;
  fds[0].fd = g->net_info.fd;
  int res = poll(fds, 1, 4);
  if (res < 0) {
    perror("poll");
    return;
  }
  if (res == 0) {
    return;
  }
  if (!(fds[0].revents & (POLLHUP | POLLIN))) {
    return;
  }
  printf("player 2 connected\n");

  struct sockaddr_storage client_addr;
  unsigned int addr_size = sizeof client_addr;
  int client_fd = accept(g->net_info.fd, (struct sockaddr*)&client_addr, &addr_size);
  if (client_fd == -1) {
    perror("accept");
    return;
  }
  g->net_info.p2_fd = client_fd;
  g->game_state = STATE_PLAY;
  game_start_new_game(g);
}

void setup_host(NetworkMultiplayerData* net) {
  struct addrinfo* addr_info = get_addr_info(net->port, nullptr);
  net->fd = open_and_listen_socket(addr_info);
  if (net->fd < 0) {
    assert(0);
    return;
  }
  freeaddrinfo(addr_info);
}

void on_host_online_game(Game* g, int port) {
  printf("hosting game on port %i\n", port);
  set_port(g, port);
  g->net_info.is_host = true;
  setup_host(&g->net_info);
  g->game_state = STATE_WAIT_FOR_PLAYER_TWO_AS_HOST;
}

void game_update_menu([[maybe_unused]] Game* g) {}

bool check_collision_rects(Rectangle r1, Rectangle r2) {
  bool horiz_overlap = fmaxf(r1.x, r2.x) > fminf(r1.x + r1.width, r2.x + r2.width);
  bool vert_overlap = fmaxf(r1.y + r1.height, r2.y + r2.height) < fminf(r1.y, r2.y);
  return horiz_overlap && vert_overlap;
}

Rectangle get_p2_paddle_rect(Game* g) {
  Vector2 paddle_half_dims = Vector2Scale(paddle_dims, 0.5f);
  return (Rectangle){world_dims.x - paddle_dims.x, g->players[1].pos - paddle_half_dims.y,
                     paddle_dims.x, paddle_dims.y};
}
Rectangle get_p1_paddle_rect(Game* g) {
  Vector2 paddle_half_dims = Vector2Scale(paddle_dims, 0.5f);
  return (Rectangle){0, -paddle_half_dims.y + g->players[0].pos, paddle_dims.x, paddle_dims.y};
}

ssize_t send_msgs(int fd, MsgHdr* hdrs, void** datas, int count) {
  // TODO: frame allocator
  struct iovec iovecs[count * 2];
  for (int i = 0; i < count; i++) {
    iovecs[i].iov_base = &hdrs[i];
    iovecs[i].iov_len = MSG_HDR_SIZE;
    iovecs[i + 1].iov_base = datas[i];
    iovecs[i + 1].iov_len = hdrs[i].len;
  }
  return writev(fd, iovecs, count * 2);
}

void game_on_msg(Game* g, Frame* fr) {
  switch (fr->hdr.type) {
    case MSG_PLAYER_POS: {
      MsgPlayerPos* u = (MsgPlayerPos*)fr->payload;
      g->players[u->player].pos = u->pos;
      break;
    }
    case MSG_SCORE_UPDATE: {
      MsgScoreUpdate* u = (MsgScoreUpdate*)fr->payload;
      g->players[u->player].score = u->score;
      break;
    }
    case MSG_BALL_POS_UPDATE: {
      MsgBall* u = (MsgBall*)fr->payload;
      g->ball_pos = u->pos;
      g->ball_velocity = u->velocity;
      break;
    }
    case MSG_STATE_UPDATE: {
      MsgStateUpdate* s = (MsgStateUpdate*)fr->payload;
      if (g->game_state == STATE_PAUSE_MENU) {
        if (s->player == g->curr_pause_player) {
          g->game_state = s->state;
          g->curr_pause_player = INT_MAX;
        }
      } else if (g->game_state == STATE_PLAY) {
        g->game_state = s->state;
        if (s->state == STATE_PAUSE_MENU) {
          g->curr_pause_player = s->player;
        }
      } else {
        assert(0 && "invalid");
      }
      break;
    }
    default:
      printf("invalid msg type: %i\n", fr->hdr.type);
      break;
  }
}

void game_process_msgs(Game* g, void* data_raw, ssize_t size) {
  ssize_t curr_read_size = 0;
  uint8_t* data = data_raw;
  while (curr_read_size < size) {
    Frame fr = {};
    memcpy(&fr.hdr.type, data + curr_read_size, sizeof(uint32_t));
    memcpy(&fr.hdr.len, data + curr_read_size + sizeof(uint32_t), sizeof(uint32_t));
    curr_read_size += MSG_HDR_SIZE;
    fr.payload = data + curr_read_size;
    curr_read_size += fr.hdr.len;
    game_on_msg(g, &fr);
  }
}

void game_read_from_other(Game* g, int other_fd) {
  struct pollfd fds[1] = {};
  fds[0].fd = other_fd;
  fds[0].events = POLLIN;
  int res = poll(fds, 1, 4);
  if (res < 0) {
    perror("poll");
  }
  if (res == 0) {
    return;
  }
  char buf[2048];
  // TODO: handle > mtu
  ssize_t read_size = read(other_fd, buf, sizeof(buf));
  if (read_size <= 0) {
    printf("disconnected or err");
    return;
  }
  game_process_msgs(g, buf, read_size);
}

void game_update_pong_process_input(Game* g) {
  int player = g->net_info.is_host ? 0 : 1;
  {  // pos
    float dt = GetFrameTime();
    float speed = 300;
    for (int i = 0; i < 2; i++) {
      g->players[i].paddle_vert_velocity = 0;
    }
    float* vert_delta = &g->players[player].paddle_vert_velocity;
    if (IsKeyDown(KEY_J)) {
      *vert_delta = speed * dt;
    }
    if (IsKeyDown(KEY_K)) {
      *vert_delta = -speed * dt;
    }
    g->players[player].pos += *vert_delta;
    if (fabsf(*vert_delta) > 0.f) {
      MsgPlayerPos p = {.pos = g->players[player].pos, .player = player};
      msg_buf_push(&g->net_info.msg_buf, MSG_PLAYER_POS, &p, sizeof(p));
    }
  }

  if (IsKeyPressed(KEY_P)) {
    g->game_state = STATE_PAUSE_MENU;
    printf("pausing game\n");
    g->curr_pause_player = get_curr_player(g);
    msg_buf_push(&g->net_info.msg_buf, MSG_STATE_UPDATE,
                 &(MsgStateUpdate){.state = g->game_state, .player = get_curr_player(g)},
                 sizeof(MsgStateUpdate));
  }
}

void game_update_pong_game_online(Game* g) {
  game_update_pong_process_input(g);
  if (g->net_info.is_host) {
    float dt = GetFrameTime();
    bool score_happened = false;
    if (g->ball_pos.x - ball_radius <= 0) {
      score_happened = true;
      g->players[0].score++;
      msg_buf_push(&g->net_info.msg_buf, MSG_SCORE_UPDATE,
                   &(MsgScoreUpdate){.score = g->players[0].score, .player = 0},
                   sizeof(MsgScoreUpdate));
    }
    if (g->ball_pos.x + ball_radius >= world_dims.x) {
      score_happened = true;
      g->players[1].score++;
      msg_buf_push(&g->net_info.msg_buf, MSG_SCORE_UPDATE,
                   &(MsgScoreUpdate){.score = g->players[1].score, .player = 1},
                   sizeof(MsgScoreUpdate));
    }
    if (score_happened) {
      game_reset_ball(g);
    }

    Rectangle circle_rect = {.x = g->ball_pos.x - ball_radius,
                             .y = g->ball_pos.y - ball_radius,
                             .width = ball_radius * 2.f,
                             .height = ball_radius * 2.f};
    Rectangle p1_rect = get_p1_paddle_rect(g);
    Rectangle p2_rect = get_p2_paddle_rect(g);
    float vert_ball_move_mult = 400.f;
    float ball_speed_x_collision_mult = ball_base_speed_x / 10.f;
    if (CheckCollisionRecs(p1_rect, circle_rect)) {
      g->collision_count++;
      g->ball_velocity.x =
          ball_base_speed_x + (float)g->collision_count * ball_speed_x_collision_mult;
      g->ball_pos.x = fmaxf(paddle_dims.x, g->ball_pos.x) + 0.1f;
      g->ball_velocity.y = g->players[0].paddle_vert_velocity * vert_ball_move_mult;
    }
    if (CheckCollisionRecs(p2_rect, circle_rect)) {
      g->collision_count++;
      g->ball_velocity.x =
          -(ball_base_speed_x + (float)g->collision_count * ball_speed_x_collision_mult);
      g->ball_pos.x = fminf(g->ball_pos.x, world_dims.x - paddle_dims.x) - 0.1f;
      g->ball_velocity.y = g->players[1].paddle_vert_velocity * vert_ball_move_mult;
    }

    g->ball_pos = Vector2Add(g->ball_pos, Vector2Scale(g->ball_velocity, dt));

    msg_buf_push(&g->net_info.msg_buf, MSG_BALL_POS_UPDATE,
                 &(MsgBall){.pos = g->ball_pos, .velocity = g->ball_velocity}, sizeof(MsgBall));
  }
}

void game_update_pong_game(Game* g) {
  if (is_online_game(g)) {
    game_update_pong_game_online(g);
    return;
  }
  if (IsKeyPressed(KEY_P)) {
    g->game_state = STATE_PAUSE_MENU;
    return;
  }

  float p1_vert_delta = 0.f, p2_vert_delta = 0.f;
  float dt = GetFrameTime();
  float speed = 300;
  if (IsKeyDown(KEY_J)) {
    p1_vert_delta = speed * dt;
  }
  if (IsKeyDown(KEY_K)) {
    p1_vert_delta = -speed * dt;
  }
  if (IsKeyDown(KEY_N)) {
    p2_vert_delta = speed * dt;
  }
  if (IsKeyDown(KEY_M)) {
    p2_vert_delta = -speed * dt;
  }
  g->players[0].pos += p1_vert_delta;
  g->players[1].pos += p2_vert_delta;

  Rectangle circle_rect = {.x = g->ball_pos.x - ball_radius,
                           .y = g->ball_pos.y - ball_radius,
                           .width = ball_radius * 2.f,
                           .height = ball_radius * 2.f};
  Rectangle p1_rect = get_p1_paddle_rect(g);
  Rectangle p2_rect = get_p2_paddle_rect(g);
  float vert_ball_move_mult = 400.f;
  float ball_speed_x_collision_mult = ball_base_speed_x / 10.f;
  if (CheckCollisionRecs(p1_rect, circle_rect)) {
    g->collision_count++;
    g->ball_velocity.x =
        ball_base_speed_x + (float)g->collision_count * ball_speed_x_collision_mult;
    g->ball_pos.x = fmaxf(paddle_dims.x, g->ball_pos.x) + 0.1f;
    g->ball_velocity.y = p1_vert_delta * vert_ball_move_mult;
  }
  if (CheckCollisionRecs(p2_rect, circle_rect)) {
    g->collision_count++;
    g->ball_velocity.x =
        -(ball_base_speed_x + (float)g->collision_count * ball_speed_x_collision_mult);
    g->ball_pos.x = fminf(g->ball_pos.x, world_dims.x - paddle_dims.x) - 0.1f;
    g->ball_velocity.y = p2_vert_delta * vert_ball_move_mult;
  }

  g->ball_pos = Vector2Add(g->ball_pos, Vector2Scale(g->ball_velocity, dt));
}

void game_update_pause_menu(Game* g) {
  if (g->curr_pause_player == get_curr_player(g) &&
      (IsKeyPressed(KEY_P) || IsKeyPressed(KEY_BACKSLASH))) {
    g->game_state = STATE_PLAY;
    g->curr_pause_player = INT_MAX;
    msg_buf_push(&g->net_info.msg_buf, MSG_STATE_UPDATE,
                 &(MsgStateUpdate){.state = g->game_state, .player = get_curr_player(g)},
                 sizeof(MsgStateUpdate));
  }
}

void game_update(Game* g) {
  void (*update_fns[STATE_COUNT])(Game*) = {
      [STATE_MENU] = game_update_menu,
      [STATE_PAUSE_MENU] = game_update_pause_menu,
      [STATE_PLAY] = game_update_pong_game,
      [STATE_WAIT_FOR_PLAYER_TWO_AS_HOST] = game_update_wait_for_player_2};
  assert(g->game_state < STATE_COUNT);

  game_read_from_other(g, get_other_player_fd(g));

  update_fns[g->game_state](g);

  msg_buf_send_and_clear(&g->net_info.msg_buf, get_other_player_fd(g));
}

void game_draw_pong(Game* g) {
  BeginMode2D(g->camera);
  Color paddle_color = GOLD;
  char buf[50];
  snprintf(buf, sizeof(buf), "P1: %i\nP2: %i", g->players[0].score, g->players[1].score);
  DrawText(buf, 0, 0, 20, ORANGE);
  snprintf(buf, sizeof(buf), "vel x: %f, y: %f", g->ball_velocity.x, g->ball_velocity.y);
  DrawText(buf, 0, 40, 20, ORANGE);
  Rectangle p1_rect = get_p1_paddle_rect(g);
  Rectangle p2_rect = get_p2_paddle_rect(g);
  DrawRectangleRec(p1_rect, paddle_color);
  DrawRectangleRec(p2_rect, paddle_color);
  DrawCircleV(g->ball_pos, ball_radius, GREEN);
  EndMode2D();
}

static long override_player = INT_MAX;

void game_draw_menu(Game* g) {
  Vector2 button_dims = {120, 40};
  float space_y = 30.f;
  typedef enum MenuState { MENU_STATE_MAIN, MENU_STATE_ONLINE_MENU } MenuState;
  static MenuState menu_state = MENU_STATE_MAIN;
  switch (menu_state) {
    case MENU_STATE_MAIN: {
      if (override_player == 0) on_host_online_game(g, 8080);
      if (override_player == 1) on_join_online_game(g, 8080, "127.0.0.1");
      if (GuiButton((Rectangle){window_dims.x / 2 - (button_dims.x / 2.f),
                                window_dims.y / 2 - (button_dims.y / 2.f) - space_y, button_dims.x,
                                button_dims.y},
                    "Local Multiplayer")) {
        on_local_game_start(g);
      }
      if (GuiButton((Rectangle){window_dims.x / 2 - (button_dims.x / 2.f),
                                window_dims.y / 2 - (button_dims.y / 2.f) + space_y, button_dims.x,
                                button_dims.y},
                    "Online Multiplayer")) {
        menu_state = MENU_STATE_ONLINE_MENU;
      }
      break;
    }
    case MENU_STATE_ONLINE_MENU: {
      if (g->net_info.error_msg) {
        DrawText(g->net_info.error_msg, 0, (int)window_dims.y / 2, 20, RED);
      }
      Vector2 half_win_dims = Vector2Scale(window_dims, 0.5f);
      float space_x = 100;

      {
        static int host_port = 0;
        static bool port_mode = false;
        Rectangle port_button_bounds = {half_win_dims.x - (button_dims.x / 2.f) - space_x,
                                        half_win_dims.y - (button_dims.y / 2.f) - space_y * 2.f,
                                        button_dims.x, button_dims.y};
        if (GuiValueBox(port_button_bounds, nullptr, &host_port, 0, 100000, port_mode)) {
          port_mode = !port_mode;
        }
        GuiDrawText(
            "Port",
            (Rectangle){port_button_bounds.x, port_button_bounds.y - (button_dims.y / 2.f + 5),
                        port_button_bounds.width, port_button_bounds.height},
            10, BLACK);
        Rectangle host_game_button_bounds = {half_win_dims.x - (button_dims.x / 2.f) - space_x,
                                             half_win_dims.y - (button_dims.y / 2.f), button_dims.x,
                                             button_dims.y};
        if (GuiButton(host_game_button_bounds, "Host game")) {
          on_host_online_game(g, host_port);
        }
      }

      static int port = 0;
      static bool port_mode = false;
      Rectangle port_button_bounds = {half_win_dims.x - (button_dims.x / 2.f) + space_x,
                                      half_win_dims.y - (button_dims.y / 2.f) - space_y * 4.f,
                                      button_dims.x, button_dims.y};
      if (GuiValueBox(port_button_bounds, nullptr, &port, 0, 100000, port_mode)) {
        port_mode = !port_mode;
      }
      GuiDrawText(
          "Host Port",
          (Rectangle){port_button_bounds.x, port_button_bounds.y - (button_dims.y / 2.f + 5),
                      port_button_bounds.width, port_button_bounds.height},
          10, BLACK);

      static char ip_addr[50] = {};
      static bool ip_addr_edit_mode = false;
      GuiDrawText(
          "Host IP Address",
          (Rectangle){half_win_dims.x - (button_dims.x / 2.f) + space_x,
                      half_win_dims.y - (button_dims.y / 2.f) - space_y - (button_dims.y / 2.f + 5),
                      button_dims.x, button_dims.y},
          10, BLACK);
      if (GuiTextBox((Rectangle){half_win_dims.x - (button_dims.x / 2.f) + space_x,
                                 half_win_dims.y - (button_dims.y / 2.f) - space_y, button_dims.x,
                                 button_dims.y},
                     ip_addr, sizeof(ip_addr), ip_addr_edit_mode)) {
        ip_addr_edit_mode = !ip_addr_edit_mode;
      }
      if (GuiButton((Rectangle){window_dims.x / 2 - (button_dims.x / 2.f) + space_x,
                                window_dims.y / 2 - (button_dims.y / 2.f) + space_y * 2.f,
                                button_dims.x, button_dims.y},
                    "Join Game")) {
        on_join_online_game(g, port, ip_addr);
      }
      break;
    }
  }
  BeginMode2D(g->camera);
  DrawText("Pong lol", 0, 0, 30, (Color){255, 0, 0, 255});
  EndMode2D();
}

void game_draw_pause_menu(Game* g) {
  BeginMode2D(g->camera);
  char buf[50];
  snprintf(buf, sizeof(buf), "Paused by player %i", g->curr_pause_player + 1);
  DrawText(buf, 0, 0, 10, (Color){255, 0, 0, 255});
  game_draw_pong(g);
  EndMode2D();
}

void game_draw_wait_for_player_two_as_host(Game* g) {
  char buf[100];
  const char* ip = "ip addr";
  snprintf(buf, sizeof(buf), "Waiting for player 2 on IP Addr %s, port %s", ip, g->net_info.port);
  DrawText(buf, 0, 0, 20, RED);
}

void game_draw(Game* g) {
  DrawRectangle(0, 0, (int)window_dims.x, (int)g->viewport.y, BLACK);
  DrawRectangle(0, (int)(g->viewport.y + g->viewport.height), (int)window_dims.x,
                (int)(window_dims.y - (g->viewport.y + g->viewport.height)), BLACK);
  DrawRectangle(0, (int)g->viewport.y, (int)g->viewport.x, (int)g->viewport.height, BLACK);
  DrawRectangle((int)(g->viewport.x + g->viewport.width), (int)g->viewport.y,
                (int)(window_dims.x - (g->viewport.x + g->viewport.width)), (int)g->viewport.height,
                BLACK);

  void (*draw_fns[STATE_COUNT])(Game*) = {
      [STATE_MENU] = game_draw_menu,
      [STATE_PAUSE_MENU] = game_draw_pause_menu,
      [STATE_PLAY] = game_draw_pong,
      [STATE_WAIT_FOR_PLAYER_TWO_AS_HOST] = game_draw_wait_for_player_two_as_host};
  assert(g->game_state < STATE_COUNT);
  draw_fns[g->game_state](g);
}

void game_shutdown([[maybe_unused]] Game* g) {
  free((void*)g->net_info.ip_addr);
  free((void*)g->net_info.port);
}

int main(int argc, char* argv[]) {
  if (argc > 1) {
    override_player = strtol(argv[1], nullptr, 0);
  }
  InitWindow((int)window_dims.x, (int)window_dims.y, "pong");
  Game game;
  game_init(&game);

  while (!WindowShouldClose()) {
    Color background_color = {255, 255, 255, 255};
    game_update(&game);
    BeginDrawing();
    ClearBackground(background_color);
    game_draw(&game);
    EndDrawing();
  }

  game_shutdown(&game);
  CloseWindow();
}
