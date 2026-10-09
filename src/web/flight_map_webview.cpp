#include "web/flight_map_webview.h"
#include "util/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <WebView2.h>

#include <cstdio>
#include <string>

struct EnvCB : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    LONG ref = 1;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (r == 0) delete this; return r; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler || riid == IID_IUnknown)
        { *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD(Invoke)(HRESULT r, ICoreWebView2Environment* e) override;
};
struct CtrlCB : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    LONG ref = 1;
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref); }
    STDMETHOD_(ULONG, Release)() override { LONG r = InterlockedDecrement(&ref); if (r == 0) delete this; return r; }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler || riid == IID_IUnknown)
        { *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD(Invoke)(HRESULT r, ICoreWebView2Controller* c) override;
};

// Self-contained Leaflet map (OpenStreetMap tiles). Aircraft are pushed in from
// the host via window.chrome.webview messages, so we are not tied to any
// third-party aircraft site.
static const char* kMapHtml = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"/>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css"/>
<style>
html,body,#map{height:100%;margin:0;padding:0;background:#0b0e13;}
.plane-icon{background:none;border:none;}
.plane{width:26px;height:26px;transform-origin:50% 50%;
  filter:drop-shadow(0 0 2px rgba(0,0,0,0.9));}
.ac-label{background:rgba(11,14,19,0.78);border:1px solid #2b8fb0;border-radius:4px;
  padding:1px 6px;color:#eaf6ff;font:700 13px/1.5 "Segoe UI",sans-serif;
  white-space:nowrap;box-shadow:0 1px 4px rgba(0,0,0,0.7);}
.ac-label:before{display:none;}
</style></head><body>
<div id="map"></div>
<script src="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js"></script>
<script>
var map = L.map('map',{zoomControl:true}).setView([39.5,-98.35],4);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
  {subdomains:'abc',maxZoom:19,
   attribution:'&copy; OpenStreetMap contributors'}).addTo(map);
var layer = L.layerGroup().addTo(map);
var fitted = false;
var markers = {};
var PLANE='<div class="plane" style="transform:rotate(HDdeg)">'
 +'<svg viewBox="0 0 24 24" width="26" height="26">'
 +'<path fill="#33ccff" stroke="#0b0e13" stroke-width="1" stroke-linejoin="round" '
 +'d="M12 2 L13.4 9 L22 13 L22 15 L13.4 13 L13.4 19 L16 21 L16 22 L12 21 '
 +'L8 22 L8 21 L10.6 19 L10.6 13 L2 15 L2 13 L10.6 9 Z"/></svg></div>';
function makeIcon(hd){
  return L.divIcon({className:'plane-icon',html:PLANE.replace('HD',hd),
    iconSize:[26,26],iconAnchor:[13,13]});
}
// Reuse markers across updates (setLatLng / rotate in place) so aircraft don't
// blink when a new snapshot arrives.
function updateAircraft(list){
  var seen={}, b=[];
  for(var i=0;i<list.length;i++){
    var a=list[i];
    if(a.la===undefined||a.lo===undefined) continue;
    var key=a.ic||a.rg||(a.la.toFixed(3)+','+a.lo.toFixed(3));
    seen[key]=1;
    var hd=(a.hd||0).toFixed(0);
    var m=markers[key];
    if(!m){
      m=L.marker([a.la,a.lo],{icon:makeIcon(hd),riseOnHover:true}).addTo(layer);
      markers[key]=m;
    } else {
      m.setLatLng([a.la,a.lo]);
      var el=m.getElement();
      if(el){ var p=el.querySelector('.plane'); if(p) p.style.transform='rotate('+hd+'deg)'; }
    }
    var label=a.fl||a.rg||a.ic||'';
    if(m._label!==label){
      m._label=label;
      if(m.getTooltip()) m.setTooltipContent(label);
      else if(label) m.bindTooltip(label,{permanent:true,direction:'right',
        offset:[15,0],className:'ac-label'});
    }
    b.push([a.la,a.lo]);
  }
  for(var k in markers){
    if(!seen[k]){ layer.removeLayer(markers[k]); delete markers[k]; }
  }
  if(!fitted&&b.length){ map.fitBounds(b,{padding:[40,40],maxZoom:10}); fitted=true; }
}
window.addEventListener('resize',function(){ map.invalidateSize(); });
if(window.chrome&&window.chrome.webview){
  window.chrome.webview.addEventListener('message',function(e){ updateAircraft(e.data); });
}
</script></body></html>)HTML";

static std::wstring toWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back((wchar_t)c);
    return w;
}

struct FlightMapWebView::Impl {
    HWND hwnd = nullptr;
    ICoreWebView2Environment* env    = nullptr;
    ICoreWebView2Controller*  ctrl   = nullptr;
    ICoreWebView2*            webview = nullptr;
    bool ready = false;

    ~Impl() {
        if (ctrl) { ctrl->Close(); ctrl->Release(); ctrl = nullptr; }
        if (env)  { env->Release();  env  = nullptr; }
    }
    void loadMap() {
        if (!webview) return;
        std::wstring html = toWide(kMapHtml);
        webview->NavigateToString(html.c_str());
    }
    void postAircraft(const std::string& json) {
        if (!webview) return;
        std::wstring w = toWide(json);
        webview->PostWebMessageAsJson(w.c_str());
    }
};

static FlightMapWebView::Impl* g_impl = nullptr;

HRESULT STDMETHODCALLTYPE EnvCB::Invoke(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || !env || !g_impl) return result ? result : E_POINTER;
    g_impl->env = env;
    env->AddRef();
    auto* cb = new CtrlCB{};
    HRESULT hr = env->CreateCoreWebView2Controller(g_impl->hwnd, cb);
    if (FAILED(hr)) { logWrite("[webview] CreateController failed: 0x%lx", (unsigned long)hr); cb->Release(); }
    return hr;
}

HRESULT STDMETHODCALLTYPE CtrlCB::Invoke(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller || !g_impl) {
        logWrite("[webview] CtrlCB failed: 0x%lx", (unsigned long)(result ? result : E_POINTER));
        return result ? result : E_POINTER;
    }
    g_impl->ctrl = controller;
    controller->AddRef();

    HRESULT hr = controller->get_CoreWebView2(&g_impl->webview);
    if (FAILED(hr)) { logWrite("[webview] get_CoreWebView2 failed: 0x%lx", (unsigned long)hr); return hr; }

    RECT r{ -32000, -32000, -31999, -31999 };
    controller->put_Bounds(r);
    controller->put_IsVisible(FALSE);

    g_impl->ready = true;
    logWrite("[webview] ready");
    g_impl->loadMap();
    return S_OK;
}

FlightMapWebView::~FlightMapWebView() {
    g_impl = nullptr;
    delete impl_;
}

void FlightMapWebView::init(void* nativeHwnd) {
    if (impl_) return;
    impl_ = new Impl{};
    impl_->hwnd = (HWND)nativeHwnd;
    g_impl = impl_;
    // OpenStreetMap blocks unidentified clients (the "Access blocked" tile
    // page). Send a User-Agent that names the app and a contact URL, as their
    // tile usage policy requires.
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--user-agent=\"AirScope/0.2.0 (+https://sarahsforge.dev/products/airscope)\"");
    auto* cb = new EnvCB{};
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, cb);
}

void FlightMapWebView::setAircraft(const std::string& json) {
    if (!impl_ || !impl_->ready || !impl_->webview) return;
    impl_->postAircraft(json);
}

void FlightMapWebView::setBounds(int x, int y, int w, int h, bool visible) {
    if (!impl_ || !impl_->ctrl) return;
    if (!IsWindow(impl_->hwnd)) return;
    // Pass coordinates as-is; ScreenToClient was causing the map
    // to anchor to screen centre instead of the parent window.
    RECT r = (w <= 0 || h <= 0 || !visible) ? RECT{-32000,-32000,-31999,-31999} : RECT{x, y, x + w, y + h};
    impl_->ctrl->put_Bounds(r);
    impl_->ctrl->put_IsVisible(visible && r.right > r.left && r.bottom > r.top);
}

bool FlightMapWebView::isReady() const { return impl_ && impl_->ready; }

#else // !_WIN32

// Non-Windows platforms have no embedded WebView2 browser. Provide no-op stubs
// so the rest of the application builds and links unchanged; the Flight Map
// panel is not drawn on these platforms (see gui_panels.cpp).
struct FlightMapWebView::Impl {};

FlightMapWebView::~FlightMapWebView() { delete impl_; }
void FlightMapWebView::init(void*) {}
void FlightMapWebView::setAircraft(const std::string&) {}
void FlightMapWebView::setBounds(int, int, int, int, bool) {}
bool FlightMapWebView::isReady() const { return false; }

#endif // _WIN32
