#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <pigpio.h>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <mutex>
#include <cmath>

#define WIDTH 480
#define HEIGHT 800
#define FPS 30
#define SERVER "http://192.168.0.102:5005"

std::mutex mtx;

// ---------- STATE ----------
struct SpotifyState {
    std::string title = "—";
    std::string artist = "—";
    std::string art_url;
    bool playing = false;
    float progress = 0;
    float duration = 1;
    SDL_Texture* art = nullptr;
};

SpotifyState spotify;
bool show_stats = false;
SDL_Texture* bg = nullptr;
SDL_Color textColor = {255,255,255};
SDL_Renderer* renderer = nullptr;
TTF_Font* FONT = nullptr;
TTF_Font* BIG = nullptr;
TTF_Font* SMALL = nullptr;

// ---------- HTTP ----------
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 600L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return readBuffer;
}

void http_post(const std::string& path, const std::string& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    std::string url = std::string(SERVER) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
}

// ---------- FETCH THREADS ----------
void fetch_spotify() {
    while (true) {
        std::string data = http_get(std::string(SERVER) + "/spotify_now");
        std::lock_guard<std::mutex> lock(mtx);
        // parse basique
        auto get_val = [&](const std::string& key)->std::string {
            auto pos = data.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = data.find(":", pos);
            auto start = data.find_first_of("\"0123456789tfr", pos + 1);
            if (data[start] == '\"') {
                auto end = data.find("\"", start + 1);
                return data.substr(start + 1, end - start - 1);
            }
            auto end = data.find(",", start);
            return data.substr(start, end - start);
        };

        spotify.title = get_val("title");
        spotify.artist = get_val("artist");
        spotify.playing = get_val("is_playing").find("true") != std::string::npos;
        spotify.progress = std::stof(get_val("progress"));
        spotify.duration = std::stof(get_val("duration"));
        spotify.art_url = get_val("art_url");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void fetch_stats(std::string& cpu, std::string& gpu, std::string& tcpu, std::string& tgpu) {
    std::string data = http_get(std::string(SERVER) + "/metrics");
    auto find = [&](const std::string& key) {
        auto pos = data.find("\"" + key + "\"");
        if (pos == std::string::npos) return std::string("n/a");
        pos = data.find(":", pos);
        auto end = data.find(",", pos);
        return data.substr(pos + 1, end - pos - 1);
    };
    cpu = find("cpu"); gpu = find("gpu");
    tcpu = find("temp_cpu"); tgpu = find("temp_gpu");
}

// ---------- UI ----------
SDL_Texture* render_text(const std::string& text, TTF_Font* font, SDL_Color color) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

void draw_progress(float ratio, int x, int y, int w, int h) {
    SDL_Rect base = {x, y, w, h};
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderFillRect(renderer, &base);
    SDL_SetRenderDrawColor(renderer, 30, 215, 96, 255);
    SDL_Rect fill = {x, y, int(w * ratio), h};
    SDL_RenderFillRect(renderer, &fill);
}

void render_spotify() {
    std::lock_guard<std::mutex> lock(mtx);
    SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
    SDL_RenderClear(renderer);

    SDL_Texture* mode = render_text("Spotify", SMALL, textColor);
    int tw, th; SDL_QueryTexture(mode, NULL, NULL, &tw, &th);
    SDL_Rect dst = {WIDTH/2 - tw/2, 20, tw, th};
    SDL_RenderCopy(renderer, mode, NULL, &dst);
    SDL_DestroyTexture(mode);

    SDL_Texture* title = render_text(spotify.title, BIG, textColor);
    SDL_QueryTexture(title, NULL, NULL, &tw, &th);
    dst = {WIDTH/2 - tw/2, 380, tw, th};
    SDL_RenderCopy(renderer, title, NULL, &dst);
    SDL_DestroyTexture(title);

    SDL_Texture* artist = render_text(spotify.artist, FONT, textColor);
    SDL_QueryTexture(artist, NULL, NULL, &tw, &th);
    dst = {WIDTH/2 - tw/2, 420, tw, th};
    SDL_RenderCopy(renderer, artist, NULL, &dst);
    SDL_DestroyTexture(artist);

    float ratio = spotify.progress / spotify.duration;
    draw_progress(ratio, 60, 470, WIDTH - 120, 10);
}

void render_stats_screen() {
    std::string cpu,gpu,tcpu,tgpu;
    fetch_stats(cpu,gpu,tcpu,tgpu);

    SDL_SetRenderDrawColor(renderer, 20, 20, 25, 255);
    SDL_RenderClear(renderer);

    std::string labels[4] = {"CPU","GPU","Temp CPU (°C)","Temp GPU (°C)"};
    std::string vals[4] = {cpu,gpu,tcpu,tgpu};
    int y = 100;
    for (int i=0;i<4;i++){
        SDL_Texture* label = render_text(labels[i], BIG, {255,255,255});
        SDL_Texture* val = render_text(vals[i], BIG, {200,200,200});
        int lw,lh,vw,vh;
        SDL_QueryTexture(label,NULL,NULL,&lw,&lh);
        SDL_QueryTexture(val,NULL,NULL,&vw,&vh);
        SDL_Rect dl={60,y,lw,lh}, dr={WIDTH-60-vw,y,vw,vh};
        SDL_RenderCopy(renderer,label,NULL,&dl);
        SDL_RenderCopy(renderer,val,NULL,&dr);
        SDL_DestroyTexture(label);
        SDL_DestroyTexture(val);
        y+=60;
    }

    SDL_Texture* info = render_text("B4: Revenir à Spotify", SMALL, {200,200,200});
    int iw,ih; SDL_QueryTexture(info,NULL,NULL,&iw,&ih);
    SDL_Rect di={WIDTH/2 - iw/2, HEIGHT - 60, iw, ih};
    SDL_RenderCopy(renderer, info, NULL, &di);
    SDL_DestroyTexture(info);
}

// ---------- GPIO ----------
const int BTN_PREV = 17;
const int BTN_PLAY = 27;
const int BTN_NEXT = 22;
const int BTN_MODE = 5;
const int ENC_A = 6;
const int ENC_B = 13;

int lastA = 0;

void gpio_thread() {
    while (true) {
        int a = gpioRead(ENC_A);
        int b = gpioRead(ENC_B);
        if (a != lastA) {
            if (a != b) http_post("/media", "{\"cmd\":\"vol_up\"}");
            else http_post("/media", "{\"cmd\":\"vol_down\"}");
            lastA = a;
        }
        if (!gpioRead(BTN_PREV)) http_post("/media", "{\"cmd\":\"prev\"}");
        if (!gpioRead(BTN_PLAY)) http_post("/media", "{\"cmd\":\"playpause\"}");
        if (!gpioRead(BTN_NEXT)) http_post("/media", "{\"cmd\":\"next\"}");
        if (!gpioRead(BTN_MODE)) show_stats = !show_stats;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

// ---------- MAIN ----------
int main() {
    gpioInitialise();
    for (int p : {BTN_PREV,BTN_PLAY,BTN_NEXT,BTN_MODE,ENC_A,ENC_B})
        gpioSetMode(p, PI_INPUT);

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    FONT = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 26);
    BIG = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 34);
    SMALL = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22);

    SDL_Window* window = SDL_CreateWindow("PiPanel", 0, 0, WIDTH, HEIGHT, SDL_WINDOW_FULLSCREEN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    std::thread(fetch_spotify).detach();
    std::thread(gpio_thread).detach();

    bool running = true;
    Uint32 lastTick = SDL_GetTicks();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        if (show_stats) render_stats_screen();
        else render_spotify();

        SDL_RenderPresent(renderer);

        Uint32 frameTime = SDL_GetTicks() - lastTick;
        if (frameTime < (1000/FPS)) SDL_Delay((1000/FPS) - frameTime);
        lastTick = SDL_GetTicks();
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    gpioTerminate();
    return 0;
}
