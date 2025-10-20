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
#include <vector>
#include <cmath>

#define W 480
#define H 800
#define FPS 30

// ⚠️ Mets l'IP de TON PC ici (serveur Windows: pi_serveur.py)
#define PC_SERVER "http://192.168.0.102:5005"
// Spotify côté Pi (pi_core.py)
#define PI_SPOTIFY "http://127.0.0.1:5050"
// Dossier des icônes (relatif au binaire)
#define ICONS_PATH "./icons"

std::mutex mtx;

// ---------- État Spotify ----------
struct SpotifyState {
    std::string title = "—";
    std::string artist = "—";
    std::string art_url;
    bool playing = false;
    float progress = 0.f;
    float duration = 1.f;
};
SpotifyState sp;

// ---------- SDL / UI ----------
SDL_Window*   window   = nullptr;
SDL_Renderer* renderer = nullptr;

TTF_Font* FONT  = nullptr; // 26
TTF_Font* BIG   = nullptr; // 34, bold
TTF_Font* SMALL = nullptr; // 22

SDL_Texture* texArt   = nullptr;  // pochette (300x300)
SDL_Texture* texBgCur = nullptr;  // fond dégradé courant
SDL_Texture* texBgPrev= nullptr;  // fond dégradé précédent (pour le fondu)

SDL_Texture* icoPrev  = nullptr;
SDL_Texture* icoNext  = nullptr;
SDL_Texture* icoPlay  = nullptr;
SDL_Texture* icoPause = nullptr;
SDL_Texture* icoMode  = nullptr;

SDL_Color textColor = {255,255,255};

// Fondu de fond
Uint32 fadeStart = 0;
const Uint32 fadeDuration = 500; // ms

// ---------- GPIO (via pigpiod_if2) ----------
const int BTN_PREV = 17;
const int BTN_PLAY = 27;
const int BTN_NEXT = 22;
const int BTN_MODE = 5;
const int ENC_A = 6;
const int ENC_B = 13;

int pigpio = -1;
int lastA  = 0;
bool show_stats = false;

// ---------- HTTP helpers ----------
static size_t WriteCb(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url, long timeout_ms=800) {
    CURL* curl = curl_easy_init();
    std::string out;
    if (!curl) return out;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return out;
}

void http_post_json(const std::string& base, const std::string& path, const std::string& json, long timeout_ms=300) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    std::string url = base + path;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
}

// ---------- Utils ----------
float clamp01(float v){ return v<0.f?0.f:(v>1.f?1.f:v); }
int   clamp255(int v){ return v<0?0:(v>255?255:v); }

float luminance(int r,int g,int b){
    return 0.299f*r + 0.587f*g + 0.114f*b;
}

// ---------- Chargement d'image depuis mémoire en texture SDL2 ----------
SDL_Texture* load_texture_from_memory(const std::vector<unsigned char>& bytes) {
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return nullptr;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return nullptr;
    SDL_Surface* conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surf);
    if (!conv) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, conv);
    SDL_FreeSurface(conv);
    return tex;
}

// ---------- Dégradé vertical vers noir ----------
SDL_Texture* make_vertical_gradient(int r, int g, int b) {
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, W, H);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(renderer, tex);

    for (int y=0; y<H; ++y) {
        float t = (float)y / (float)H; // 0 en haut → 1 en bas
        int rr = clamp255((int)(r * (1.0f - t)));
        int gg = clamp255((int)(g * (1.0f - t)));
        int bb = clamp255((int)(b * (1.0f - t)));
        SDL_SetRenderDrawColor(renderer, rr, gg, bb, 255);
        SDL_RenderDrawLine(renderer, 0, y, W, y);
    }

    SDL_SetRenderTarget(renderer, nullptr);
    return tex;
}

// ---------- Couleur dominante (moyenne simple des pixels) ----------
bool compute_average_rgb(SDL_Texture* tex, int& r, int& g, int& b) {
    if (!tex) return false;
    // On rend la texture dans une petite cible 32x32 pour échantillonner
    SDL_Texture* tgt = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 32, 32);
    SDL_SetRenderTarget(renderer, tgt);
    SDL_RenderCopy(renderer, tex, nullptr, nullptr);
    SDL_SetRenderTarget(renderer, nullptr);

    // On récupère les pixels
    std::vector<unsigned char> pixels(32*32*4);
    SDL_SetRenderTarget(renderer, tgt);
    SDL_Rect rect{0,0,32,32};
    SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_RGBA8888, pixels.data(), 32*4);
    SDL_SetRenderTarget(renderer, nullptr);

    long long R=0,G=0,B=0, count=0;
    for (int i=0;i<32*32;i++){
        unsigned char* p = &pixels[i*4];
        R += p[0]; G += p[1]; B += p[2]; // RGBA8888
        count++;
    }
    SDL_DestroyTexture(tgt);
    if (count==0) return false;
    r = (int)(R/count); g = (int)(G/count); b = (int)(B/count);
    return true;
}

// ---------- Téléchargement pochette + MAJ fond + couleur texte ----------
void apply_art_and_background(const std::vector<unsigned char>& bytes) {
    // libère l'ancienne pochette
    if (texArt){ SDL_DestroyTexture(texArt); texArt=nullptr; }

    // crée texture pochette
    SDL_Texture* raw = load_texture_from_memory(bytes);
    if (!raw) {
        // fallback : pas d'art → fond gris sombre, texte blanc
        if (texBgCur) SDL_DestroyTexture(texBgCur);
        texBgCur = make_vertical_gradient(20,20,20);
        textColor = {255,255,255};
        return;
    }

    // Redimensionne en 300x300
    texArt = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 300, 300);
    SDL_SetTextureBlendMode(texArt, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(renderer, texArt);
    SDL_RenderCopy(renderer, raw, nullptr, nullptr);
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_DestroyTexture(raw);

    // Couleur dominante
    int r=20,g=20,b=20;
    if (!compute_average_rgb(texArt, r,g,b)) { r=g=b=20; }

    // Détermine couleur du texte
    textColor = (luminance(r,g,b) < 128.f) ? SDL_Color{255,255,255} : SDL_Color{20,20,20};

    // Prépare un nouveau fond dégradé → fondu
    SDL_Texture* newBg = make_vertical_gradient(r,g,b);
    if (texBgPrev) SDL_DestroyTexture(texBgPrev);
    texBgPrev = texBgCur;            // ancien fond
    texBgCur  = newBg;               // nouveau fond
    fadeStart = SDL_GetTicks();      // lance le fondu
}

// ---------- Thread Spotify : état + pochette ----------
void spotify_thread() {
    std::string lastArtUrl;
    while (true) {
        std::string data = http_get(std::string(PI_SPOTIFY) + "/spotify_now");
        std::lock_guard<std::mutex> lock(mtx);

        auto val = [&](const std::string& key)->std::string{
            auto pos = data.find("\""+key+"\"");
            if (pos==std::string::npos) return "";
            pos = data.find(":", pos);
            auto start = data.find_first_of("\"0123456789tf", pos+1);
            if (start==std::string::npos) return "";
            if (data[start]=='\"') {
                auto end = data.find("\"", start+1);
                return data.substr(start+1, end-start-1);
            } else {
                auto end = data.find(",", start);
                return data.substr(start, end - start);
            }
        };

        sp.title    = val("title");
        sp.artist   = val("artist");
        sp.playing  = val("is_playing").find("true") != std::string::npos;
        try {
            auto p = val("progress"); auto d = val("duration");
            sp.progress = p.empty()?0.f:std::stof(p);
            sp.duration = d.empty()?1.f:std::stof(d);
        } catch (...) { sp.progress=0; sp.duration=1; }

        std::string art = val("art_url");
        if (!art.empty() && art != lastArtUrl) {
            // télécharge l'image (bloquant court) puis applique
            CURL* curl = curl_easy_init();
            std::vector<unsigned char> buf;
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, art.c_str());
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                    +[](void* c,size_t s,size_t n,void* u)->size_t{
                        auto* v = static_cast<std::vector<unsigned char>*>(u);
                        size_t bytes = s*n; v->insert(v->end(), (unsigned char*)c, (unsigned char*)c+bytes);
                        return bytes;
                    });
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
                curl_easy_perform(curl);
                curl_easy_cleanup(curl);
            }
            apply_art_and_background(buf);
            lastArtUrl = art;
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ---------- Stats PC ----------
void fetch_metrics(std::string& cpu,std::string& gpu,std::string& tcpu,std::string& tgpu){
    std::string data = http_get(std::string(PC_SERVER)+"/metrics");
    auto f=[&](const std::string& key){
        auto p=data.find("\""+key+"\"");
        if(p==std::string::npos) return std::string("n/a");
        p=data.find(":",p);
        auto e=data.find(",",p);
        return data.substr(p+1, e-(p+1));
    };
    cpu=f("cpu"); gpu=f("gpu"); tcpu=f("temp_cpu"); tgpu=f("temp_gpu");
}

// ---------- Rendu ----------
SDL_Texture* render_text(const std::string& s, TTF_Font* font, SDL_Color c){
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

void draw_progress(float ratio, int x,int y,int w,int h){
    SDL_Rect base={x,y,w,h};
    SDL_SetRenderDrawColor(renderer, 25,25,25,255);
    SDL_RenderFillRect(renderer, &base);
    SDL_SetRenderDrawColor(renderer, 30,215,96,255);
    SDL_Rect fill={x,y,(int)(w*clamp01(ratio)),h};
    SDL_RenderFillRect(renderer, &fill);
}

void render_background() {
    // Fond & fondu
    if (texBgCur) {
        if (texBgPrev) {
            Uint32 now = SDL_GetTicks();
            float a = clamp01((now - fadeStart) / (float)fadeDuration);
            // dessine prev puis cur avec alpha
            SDL_SetTextureAlphaMod(texBgPrev, 255);
            SDL_RenderCopy(renderer, texBgPrev, nullptr, nullptr);
            SDL_SetTextureAlphaMod(texBgCur, (Uint8)(a*255));
            SDL_RenderCopy(renderer, texBgCur, nullptr, nullptr);
            if (a >= 1.f) { SDL_DestroyTexture(texBgPrev); texBgPrev = nullptr; }
        } else {
            SDL_RenderCopy(renderer, texBgCur, nullptr, nullptr);
        }
    } else {
        SDL_SetRenderDrawColor(renderer, 18,18,18,255);
        SDL_RenderClear(renderer);
    }
}

void render_spotify_screen() {
    std::lock_guard<std::mutex> lock(mtx);

    render_background();

    // "Spotify"
    SDL_Texture* tMode = render_text("Spotify", SMALL, textColor);
    int tw,th; SDL_QueryTexture(tMode,nullptr,nullptr,&tw,&th);
    SDL_Rect dMode = { W/2 - tw/2, 20, tw, th };
    SDL_RenderCopy(renderer, tMode, nullptr, &dMode);
    SDL_DestroyTexture(tMode);

    // Pochette
    if (texArt) {
        SDL_Rect dst = { W/2 - 150, 60, 300, 300 };
        SDL_RenderCopy(renderer, texArt, nullptr, &dst);
    } else {
        SDL_Texture* t = render_text("Chargement de l'album...", SMALL, {200,200,200});
        SDL_QueryTexture(t,nullptr,nullptr,&tw,&th);
        SDL_Rect d = { W/2 - tw/2, 220, tw, th };
        SDL_RenderCopy(renderer, t, nullptr, &d);
        SDL_DestroyTexture(t);
    }

    // Titre + artiste
    SDL_Texture* tTitle = render_text(sp.title.empty()?"—":sp.title, BIG, textColor);
    SDL_QueryTexture(tTitle,nullptr,nullptr,&tw,&th);
    SDL_Rect dTitle = { W/2 - tw/2, 380, tw, th };
    SDL_RenderCopy(renderer, tTitle, nullptr, &dTitle);
    SDL_DestroyTexture(tTitle);

    SDL_Texture* tArtist = render_text(sp.artist.empty()?"—":sp.artist, FONT, textColor);
    SDL_QueryTexture(tArtist,nullptr,nullptr,&tw,&th);
    SDL_Rect dArtist = { W/2 - tw/2, 410, tw, th };
    SDL_RenderCopy(renderer, tArtist, nullptr, &dArtist);
    SDL_DestroyTexture(tArtist);

    // Barre progression
    float ratio = (sp.duration>0.f) ? (sp.progress/sp.duration) : 0.f;
    draw_progress(ratio, 60, 450, W-120, 8);

    auto ms_str=[&](int ms){
        int s = ms/1000; int m=s/60; s%=60;
        char buf[8]; snprintf(buf,sizeof(buf),"%d:%02d",m,s);
        return std::string(buf);
    };

    SDL_Texture* t1 = render_text(ms_str((int)sp.progress), SMALL, {230,230,230});
    SDL_Texture* t2 = render_text(ms_str((int)sp.duration), SMALL, {230,230,230});
    int w1,h1,w2,h2;
    SDL_QueryTexture(t1,nullptr,nullptr,&w1,&h1);
    SDL_QueryTexture(t2,nullptr,nullptr,&w2,&h2);
    SDL_Rect d1={60, 450+12, w1, h1};
    SDL_Rect d2={W-60-w2, 450+12, w2, h2};
    SDL_RenderCopy(renderer,t1,nullptr,&d1);
    SDL_RenderCopy(renderer,t2,nullptr,&d2);
    SDL_DestroyTexture(t1); SDL_DestroyTexture(t2);

    // Icônes commandes
    int centerY = 520;
    if (icoPrev)  { SDL_Rect r={ W/2 - 150, centerY, 64, 64 }; SDL_RenderCopy(renderer, icoPrev,  nullptr, &r); }
    if (sp.playing) {
        if (icoPause){ SDL_Rect r={ W/2 - 32, centerY, 64, 64 }; SDL_RenderCopy(renderer, icoPause, nullptr, &r); }
    } else {
        if (icoPlay) { SDL_Rect r={ W/2 - 32, centerY, 64, 64 }; SDL_RenderCopy(renderer, icoPlay,  nullptr, &r); }
    }
    if (icoNext)  { SDL_Rect r={ W/2 + 86, centerY, 64, 64 }; SDL_RenderCopy(renderer, icoNext,  nullptr, &r); }
    if (icoMode)  { SDL_Rect r={ W/2 - 32, 640, 64, 64 }; SDL_RenderCopy(renderer, icoMode,  nullptr, &r); }
}

void render_stats_screen(){
    // Fond
    SDL_SetRenderDrawColor(renderer, 20,20,25,255);
    SDL_RenderClear(renderer);

    std::string cpu,gpu,tcpu,tgpu;
    fetch_metrics(cpu,gpu,tcpu,tgpu);

    std::string labels[4]={"CPU","GPU","Temp CPU (°C)","Temp GPU (°C)"};
    std::string vals[4]={cpu,gpu,tcpu,tgpu};
    int y=100;
    for(int i=0;i<4;i++){
        SDL_Texture* L=render_text(labels[i], BIG, {255,255,255});
        SDL_Texture* V=render_text(vals[i],   BIG, {200,200,200});
        int lw,lh,vw,vh;
        SDL_QueryTexture(L,nullptr,nullptr,&lw,&lh);
        SDL_QueryTexture(V,nullptr,nullptr,&vw,&vh);
        SDL_Rect dl={60,y,lw,lh}, dr={W-60-vw,y,vw,vh};
        SDL_RenderCopy(renderer,L,nullptr,&dl);
        SDL_RenderCopy(renderer,V,nullptr,&dr);
        SDL_DestroyTexture(L); SDL_DestroyTexture(V);
        y+=60;
    }

    SDL_Texture* info=render_text("B4: Revenir à Spotify", SMALL, {200,200,200});
    int iw,ih; SDL_QueryTexture(info,nullptr,nullptr,&iw,&ih);
    SDL_Rect di={W/2 - iw/2, H - 60, iw, ih};
    SDL_RenderCopy(renderer, info, nullptr, &di);
    SDL_DestroyTexture(info);
}

// ---------- GPIO thread ----------
void gpio_thread(){
    while(true){
        int a = gpio_read(pigpio, ENC_A);
        int b = gpio_read(pigpio, ENC_B);
        if (a != lastA) {
            if (a != b) http_post_json(PC_SERVER, "/media", "{\"cmd\":\"vol_up\"}");
            else        http_post_json(PC_SERVER, "/media", "{\"cmd\":\"vol_down\"}");
            lastA = a;
        }
        if (!gpio_read(pigpio, BTN_PREV))  http_post_json(PC_SERVER,  "/media",        "{\"cmd\":\"prev\"}");
        if (!gpio_read(pigpio, BTN_NEXT))  http_post_json(PC_SERVER,  "/media",        "{\"cmd\":\"next\"}");
        if (!gpio_read(pigpio, BTN_PLAY))  http_post_json(PI_SPOTIFY, "/spotify_cmd",  "{\"cmd\":\"playpause\"}");
        if (!gpio_read(pigpio, BTN_MODE))  show_stats = !show_stats;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
}

// ---------- Chargement icônes ----------
SDL_Texture* load_icon(const std::string& name){
    std::string path = std::string(ICONS_PATH) + "/" + name;
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    return t;
}

// ---------- MAIN ----------
int main(){
    // pigpio daemon
    pigpio = pigpio_start(NULL, NULL);
    if (pigpio < 0) {
        std::cerr << "Erreur : impossible de se connecter à pigpiod (lancé ? pigpiod -n 127.0.0.1)" << std::endl;
        return 1;
    }
    for (int p : {BTN_PREV,BTN_PLAY,BTN_NEXT,BTN_MODE,ENC_A,ENC_B})
        set_mode(pigpio, p, PI_INPUT);

    // SDL
    if (SDL_Init(SDL_INIT_VIDEO)!=0) { std::cerr<<"SDL_Init error\n"; return 1; }
    if (TTF_Init()!=0) { std::cerr<<"TTF_Init error\n"; return 1; }
    if (!(IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG) & (IMG_INIT_PNG|IMG_INIT_JPG))) {
        std::cerr<<"IMG_Init error\n"; return 1;
    }

    FONT  = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 26);
    BIG   = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 34);
    SMALL = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22);

    window   = SDL_CreateWindow("PiPanel", 0, 0, W, H, SDL_WINDOW_FULLSCREEN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Icônes
    icoPrev  = load_icon("prev.png");
    icoNext  = load_icon("next.png");
    icoPlay  = load_icon("play.png");
    icoPause = load_icon("pause.png");
    icoMode  = load_icon("mode.png");

    // Fond par défaut sombre + texte blanc
    texBgCur = make_vertical_gradient(20,20,20);
    textColor = {255,255,255};

    std::thread(spotify_thread).detach();
    std::thread(gpio_thread).detach();

    bool running=true;
    Uint32 lastTick=SDL_GetTicks(), lastResync=SDL_GetTicks();

    while(running){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if (e.type==SDL_QUIT) running=false;
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) running=false;
        }

        // Progression fluide (+ resync léger toutes 5s)
        Uint32 now = SDL_GetTicks();
        Uint32 dt = now - lastTick; lastTick = now;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (sp.playing) {
                sp.progress += dt;
                if (sp.progress > sp.duration) sp.progress = sp.duration;
            }
        }
        if (now - lastResync > 5000) { /* la thread Spotify resynchronise déjà */ lastResync = now; }

        if (show_stats) render_stats_screen();
        else            render_spotify_screen();

        SDL_RenderPresent(renderer);
        // cadence
        Uint32 frame = SDL_GetTicks() - now;
        if (frame < (1000/FPS)) SDL_Delay((1000/FPS) - frame);
    }

    // Cleanup
    if (texArt) SDL_DestroyTexture(texArt);
    if (texBgCur) SDL_DestroyTexture(texBgCur);
    if (texBgPrev) SDL_DestroyTexture(texBgPrev);
    for (auto t : {icoPrev,icoNext,icoPlay,icoPause,icoMode}) if (t) SDL_DestroyTexture(t);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    pigpio_stop(pigpio);
    return 0;
}
