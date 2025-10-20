#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>
#include <pigpiod_if2.h>
#include <string>
#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>

#define WIDTH 480
#define HEIGHT 800
#define FPS 30
#define PC_SERVER "http://192.168.0.102:5005"  // <-- IP du PC
#define PI_SPOTIFY "http://127.0.0.1:5050"
#define ICONS_PATH "./icons"

// ---------- ÉTAT ----------
struct SpotifyState {
    std::string title = "—";
    std::string artist = "—";
    bool playing = false;
    float progress = 0;
    float duration = 1;
};
SpotifyState spotify;
bool show_stats = false;

std::mutex mtx;
SDL_Renderer* renderer = nullptr;
SDL_Texture* frame = nullptr;
TTF_Font* FONT = nullptr;
TTF_Font* BIG = nullptr;
TTF_Font* SMALL = nullptr;
SDL_Color textColor = {255,255,255};

// ---------- GPIO ----------
const int BTN_PREV = 17;
const int BTN_PLAY = 27;
const int BTN_NEXT = 22;
const int BTN_MODE = 5;
const int ENC_A = 6;
const int ENC_B = 13;
int pigpio;
int lastA = 0;

// ---------- HTTP ----------
size_t WriteCallback(void* c, size_t s, size_t n, std::string* out) {
    out->append((char*)c, s*n);
    return s*n;
}
std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string buf;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 800L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return buf;
}
void http_post(const std::string& base, const std::string& path, const std::string& json) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    std::string url = base + path;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
}

// ---------- THREAD SPOTIFY ----------
void spotify_thread() {
    while (true) {
        std::string data = http_get(std::string(PI_SPOTIFY)+"/spotify_now");
        std::lock_guard<std::mutex> lock(mtx);
        auto val=[&](std::string key)->std::string{
            auto p=data.find("\""+key+"\"");
            if(p==std::string::npos)return "";
            p=data.find(":",p);
            auto s=data.find_first_of("\"0123456789tf",p+1);
            if(data[s]=='\"'){auto e=data.find("\"",s+1);return data.substr(s+1,e-s-1);}
            auto e=data.find(",",s);return data.substr(s,e-s);
        };
        spotify.title  = val("title");
        spotify.artist = val("artist");
        spotify.playing= val("is_playing").find("true")!=std::string::npos;
        try{
            spotify.progress=std::stof(val("progress"));
            spotify.duration=std::stof(val("duration"));
        }catch(...){spotify.progress=0;spotify.duration=1;}
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ---------- UI ----------
SDL_Texture* render_text(const std::string& s, TTF_Font* f, SDL_Color c){
    SDL_Surface* surf=TTF_RenderUTF8_Blended(f,s.c_str(),c);
    SDL_Texture* t=SDL_CreateTextureFromSurface(renderer,surf);
    SDL_FreeSurface(surf);
    return t;
}
void draw_progress(SDL_Renderer* r,float ratio,int x,int y,int w,int h){
    SDL_Rect bg={x,y,w,h};
    SDL_SetRenderDrawColor(r,30,30,30,255);
    SDL_RenderFillRect(r,&bg);
    SDL_SetRenderDrawColor(r,30,215,96,255);
    SDL_Rect fill={x,y,int(w*ratio),h};
    SDL_RenderFillRect(r,&fill);
}
void render_spotify(SDL_Renderer* r){
    std::lock_guard<std::mutex> lock(mtx);
    SDL_SetRenderDrawColor(r,18,18,18,255);
    SDL_RenderClear(r);

    SDL_Texture* mode=render_text("Spotify",SMALL,textColor);
    int tw,th; SDL_QueryTexture(mode,nullptr,nullptr,&tw,&th);
    SDL_Rect dm={WIDTH/2-tw/2,20,tw,th};
    SDL_RenderCopy(r,mode,nullptr,&dm);
    SDL_DestroyTexture(mode);

    SDL_Texture* title=render_text(spotify.title,BIG,textColor);
    SDL_QueryTexture(title,nullptr,nullptr,&tw,&th);
    SDL_Rect dt={WIDTH/2-tw/2,380,tw,th};
    SDL_RenderCopy(r,title,nullptr,&dt);
    SDL_DestroyTexture(title);

    SDL_Texture* art=render_text(spotify.artist,FONT,textColor);
    SDL_QueryTexture(art,nullptr,nullptr,&tw,&th);
    SDL_Rect da={WIDTH/2-tw/2,420,tw,th};
    SDL_RenderCopy(r,art,nullptr,&da);
    SDL_DestroyTexture(art);

    draw_progress(r,spotify.progress/spotify.duration,60,460,WIDTH-120,8);
}
void render_stats(SDL_Renderer* r){
    SDL_SetRenderDrawColor(r,20,20,25,255);
    SDL_RenderClear(r);
    std::string data=http_get(std::string(PC_SERVER)+"/metrics");
    auto f=[&](std::string key){
        auto p=data.find("\""+key+"\"");
        if(p==std::string::npos)return std::string("n/a");
        p=data.find(":",p);auto e=data.find(",",p);
        return data.substr(p+1,e-p-1);
    };
    std::string cpu=f("cpu"),gpu=f("gpu"),tcpu=f("temp_cpu"),tgpu=f("temp_gpu");
    std::string lbl[4]={"CPU","GPU","Temp CPU (°C)","Temp GPU (°C)"};
    std::string val[4]={cpu,gpu,tcpu,tgpu};
    int y=100;
    for(int i=0;i<4;i++){
        SDL_Texture* l=render_text(lbl[i],BIG,{255,255,255});
        SDL_Texture* v=render_text(val[i],BIG,{200,200,200});
        int lw,lh,vw,vh;
        SDL_QueryTexture(l,nullptr,nullptr,&lw,&lh);
        SDL_QueryTexture(v,nullptr,nullptr,&vw,&vh);
        SDL_Rect dl={60,y,lw,lh},dr={WIDTH-60-vw,y,vw,vh};
        SDL_RenderCopy(r,l,nullptr,&dl);
        SDL_RenderCopy(r,v,nullptr,&dr);
        SDL_DestroyTexture(l);SDL_DestroyTexture(v);
        y+=60;
    }
}

// ---------- GPIO Thread ----------
void gpio_thread() {
    bool modePressed = false;
    while (true) {
        int a = gpio_read(pigpio, ENC_A);
        int b = gpio_read(pigpio, ENC_B);
        if (a != lastA) {
            if (a != b) http_post(PC_SERVER, "/media", "{\"cmd\":\"vol_up\"}");
            else        http_post(PC_SERVER, "/media", "{\"cmd\":\"vol_down\"}");
            lastA = a;
        }

        // Lecture boutons avec debounce
        auto btnRead = [&](int pin){
            static std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
            static bool lastState=true;
            bool state=gpio_read(pigpio,pin);
            if(state!=lastState){
                last=std::chrono::steady_clock::now();
                lastState=state;
            }
            if(!state && (std::chrono::steady_clock::now()-last)>std::chrono::milliseconds(100))
                return true;
            return false;
        };

        if (btnRead(BTN_PREV)) http_post(PC_SERVER, "/media", "{\"cmd\":\"prev\"}");
        if (btnRead(BTN_NEXT)) http_post(PC_SERVER, "/media", "{\"cmd\":\"next\"}");
        if (btnRead(BTN_PLAY)) http_post(PI_SPOTIFY, "/spotify_cmd", "{\"cmd\":\"playpause\"}");

        bool pressed = !gpio_read(pigpio, BTN_MODE);
        if (pressed && !modePressed) {
            show_stats = !show_stats;
            modePressed = true;
        }
        if (!pressed) modePressed = false;

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

// ---------- MAIN ----------
int main() {
    // pigpio
    pigpio = pigpio_start(NULL, NULL);
    if (pigpio < 0) {
        std::cerr << "Erreur pigpio (lancé ? sudo pigpiod -n 127.0.0.1)\n";
        return 1;
    }
    for (int p : {BTN_PREV,BTN_PLAY,BTN_NEXT,BTN_MODE,ENC_A,ENC_B})
        set_mode(pigpio, p, PI_INPUT);

    // SDL
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    FONT = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 26);
    BIG  = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 34);
    SMALL= TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22);

    SDL_Window* win = SDL_CreateWindow("PiPanel", 0, 0, WIDTH, HEIGHT, SDL_WINDOW_FULLSCREEN);
    renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, WIDTH, HEIGHT);

    std::thread(spotify_thread).detach();
    std::thread(gpio_thread).detach();

    bool run = true;
    Uint32 last = SDL_GetTicks();
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type==SDL_QUIT) run=false;
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) run=false;
        }

        Uint32 now = SDL_GetTicks();
        Uint32 dt = now-last; last=now;
        if (spotify.playing) {
            spotify.progress += dt;
            if (spotify.progress > spotify.duration) spotify.progress = spotify.duration;
        }

        // rendu sur texture frame
        SDL_SetRenderTarget(renderer, frame);
        if (show_stats) render_stats(renderer);
        else render_spotify(renderer);
        SDL_SetRenderTarget(renderer, nullptr);

        // rotation 90° avant affichage
        SDL_RenderClear(renderer);
        SDL_RenderCopyEx(renderer, frame, NULL, NULL, 90, NULL, SDL_FLIP_NONE);
        SDL_RenderPresent(renderer);

        SDL_Delay(1000/FPS);
    }

    pigpio_stop(pigpio);
    SDL_DestroyTexture(frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
