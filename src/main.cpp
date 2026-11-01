#include "jxrc/converter.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
namespace {
void help() {
    std::cout << "jxrconvert " << jxrc::version() << R"( - LocalViewer image conversion library/CLI
Usage: jxrconvert INPUT OUTPUT --target TARGET [OPTIONS]
Inputs: JPEG, AVIF, JXL, PNG, WebP, TIFF, BMP, GIF, JPEG XR and LibRaw formats
Targets: jpeg, jpeg-gainmap, jxl, jxl-linear, jxl-pq, avif, avif-pq, avif-gainmap
  --mode preserve|process       Default: preserve; no automatic processing fallback
  --preset lossless|high|medium|low|custom    Default: high
  --quality 1..100              Custom JPEG/AVIF quality (selects custom preset)
  --jxl-distance 0..3           Custom JXL distance (selects custom preset)
  --avif-depth 10|12            Custom AVIF maximum depth (selects custom preset)
  --downsize-gainmap            Halve gain-map size (selects custom preset)
  --jpeg-transcoding           Preserve JPEG coefficients in JXL (preserve mode only)
  --threads 0..65535            Per-conversion CPU budget; 0 = automatic
  --keep-exif                   Keep source EXIF where supported
  --overwrite                   Replace existing output after successful encoding
Processing controls:
  --exposure -3..3              Linear-light exposure in stops
  --max-edge PIXELS             Resize longest edge; 0 disables resizing
  --convert-gamut              Convert scRGB to the BT.2020 working gamut
  --no-extended-bt2020          Disable extended BT.2020 working conversion for JXL
RAW controls (process mode):
  --raw-mode source|boost|custom  Source headroom, default boost, or custom controls
  --raw-exposure -3..3          Exposure in stops
  --raw-wb camera|auto|daylight|cloudy|shade|tungsten|fluorescent|flash|kelvin
  --raw-kelvin 2000..12000      Used with --raw-wb kelvin
  --raw-highlight-stops 0..3    Highlight expansion
  --raw-shoulder 0..1           Highlight shoulder
  --raw-target-nits 1000..10000 HDR ceiling
  --raw-shadows -1..1 --raw-midtones -1..1 --raw-highlights -1..1
  --raw-no-camera-look          Disable SDR camera look
  --raw-linear-hdr              Bypass camera look in HDR
Lossless is a codec preset; JPEG remains lossy. Preserve retains source transfer,
profiles and transferable gain maps; it does not promise bit-identical RGB.
)";
}
float number(const std::string& s) {
    size_t n=0;float v=std::stof(s,&n);if(n!=s.size()||!std::isfinite(v))throw std::invalid_argument("Invalid number: "+s);return v;
}
int integer(const std::string& s) {
    size_t n=0;int v=std::stoi(s,&n);if(n!=s.size())throw std::invalid_argument("Invalid integer: "+s);return v;
}
template<class T> T choice(const std::string& s,const std::map<std::string,T>& values) {
    auto it=values.find(s);if(it==values.end())throw std::invalid_argument("Unknown selection: "+s);return it->second;
}
int run(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--help"){help();return 0;}
    if(argc==2&&std::string(argv[1])=="--version"){std::cout<<jxrc::version()<<'\n';return 0;}
    if(argc<3){help();return 2;}
    jxrc::Options o;bool target=false,custom=false,preset=false,raw_custom=false,raw_mode=false;
    for(int i=3;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg.rfind("--raw-",0)==0&&arg!="--raw-mode")raw_custom=true;
        auto value=[&]()->std::string{if(++i>=argc)throw std::invalid_argument("Missing value for "+arg);return argv[i];};
        using T=jxrc::Target;using P=jxrc::Preset;
        if(arg=="--target") {o.target=choice<T>(value(),{{"jpeg",T::jpeg},{"jpeg-gainmap",T::jpeg_gainmap},{"jxl",T::jxl},{"jxl-linear",T::jxl_linear},{"jxl-pq",T::jxl_pq},{"avif",T::avif},{"avif-pq",T::avif_pq},{"avif-gainmap",T::avif_gainmap}});target=true;}
        else if(arg=="--mode")o.mode=choice<jxrc::Mode>(value(),{{"preserve",jxrc::Mode::preserve},{"process",jxrc::Mode::process}});
        else if(arg=="--preset"){o.quality.preset=choice<P>(value(),{{"lossless",P::lossless},{"high",P::high},{"medium",P::medium},{"low",P::low},{"custom",P::custom}});preset=true;}
        else if(arg=="--quality"){o.quality.codec_quality=integer(value());custom=true;}
        else if(arg=="--jxl-distance"){o.quality.jxl_distance=number(value());custom=true;}
        else if(arg=="--avif-depth"){o.quality.avif_depth=integer(value());custom=true;}
        else if(arg=="--downsize-gainmap"){o.quality.downsize_gainmap=true;custom=true;}
        else if(arg=="--jpeg-transcoding")o.jpeg_transcoding=true;
        else if(arg=="--threads"){int n=integer(value());if(n<0)throw std::invalid_argument("threads must be nonnegative");o.threads=unsigned(n);}
        else if(arg=="--keep-exif")o.keep_exif=true;
        else if(arg=="--overwrite")o.overwrite=true;
        else if(arg=="--convert-gamut")o.convert_gamut=true;
        else if(arg=="--no-extended-bt2020")o.extended_bt2020=false;
        else if(arg=="--exposure")o.exposure=number(value());
        else if(arg=="--max-edge"){const int v=integer(value());if(v<0)throw std::invalid_argument("max-edge must be nonnegative");o.max_edge=v;}
        else if(arg=="--raw-mode"){o.raw.hdr_mode=choice<jxrc::RawHdrMode>(value(),{{"source",jxrc::RawHdrMode::source},{"boost",jxrc::RawHdrMode::boost},{"custom",jxrc::RawHdrMode::custom}});raw_mode=true;}
        else if(arg=="--raw-exposure")o.raw.exposure=number(value());
        else if(arg=="--raw-wb")o.raw.white_balance=choice<int>(value(),{{"camera",0},{"auto",1},{"daylight",2},{"cloudy",3},{"shade",4},{"tungsten",5},{"fluorescent",6},{"flash",7},{"kelvin",8}});
        else if(arg=="--raw-kelvin")o.raw.kelvin=integer(value());
        else if(arg=="--raw-highlight-stops")o.raw.highlight_stops=number(value());
        else if(arg=="--raw-shoulder")o.raw.shoulder=number(value());
        else if(arg=="--raw-target-nits")o.raw.target_nits=integer(value());
        else if(arg=="--raw-shadows")o.raw.shadows=number(value());
        else if(arg=="--raw-midtones")o.raw.midtones=number(value());
        else if(arg=="--raw-highlights")o.raw.highlights=number(value());
        else if(arg=="--raw-no-camera-look")o.raw.camera_look=false;
        else if(arg=="--raw-linear-hdr")o.raw.linear_hdr=true;
        else throw std::invalid_argument("Unknown option: "+arg);
    }
    if(raw_custom&&!raw_mode)o.raw.hdr_mode=jxrc::RawHdrMode::custom;
    if(!target)throw std::invalid_argument("--target is required");
    if(custom) {if(preset&&o.quality.preset!=jxrc::Preset::custom)throw std::invalid_argument("Custom quality flags conflict with the selected preset");o.quality.preset=jxrc::Preset::custom;}
    auto result=jxrc::convert(std::filesystem::u8path(argv[1]),std::filesystem::u8path(argv[2]),o);
    std::cout<<result.width<<'x'<<result.height<<" "<<result.encoding<<" -> "<<argv[2]<<'\n';return 0;
}
}
int main(int argc,char** argv) {
    try {return run(argc,argv);}
    catch(const std::invalid_argument& e){std::cerr<<"Invalid arguments: "<<e.what()<<'\n';return 2;}
    catch(const std::out_of_range& e){std::cerr<<"Invalid numeric argument: "<<e.what()<<'\n';return 2;}
    catch(const std::exception& e){std::cerr<<"Conversion failed: "<<e.what()<<'\n';return 1;}
}
