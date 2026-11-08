#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Params.h"

// Factory presets, written in real units (metres, seconds, dB) through the inverse maps
// of Params.h. Anything not listed keeps its default. UTF-8 names.
// The first one is what a new instance starts with: made to drop on a master bus.
namespace silt::presets {

struct Preset {
    const char* category;
    const char* name;
    const char* note;  // one line shown under the preset name
    std::vector<std::pair<std::string, float>> values;
};

inline const std::vector<Preset>& all() {
    using namespace params;
    constexpr float Mud = 0, Sand = 1, Gravel = 2, Rock = 3, Reef = 4;
    constexpr float Cliff = 0, ReefWall = 1, Hull = 2, Ice = 3;
    constexpr float OpenSea = 0, Bay = 1, Pool = 2, Cave = 3, Wreck = 4, Tank = 5, Abyss = 6;
    constexpr float Hydrophone = 0, Diver = 1, Helmet = 2, Cinema = 3;
    static const std::vector<Preset> list = {
        {"MASTER", "Sous l'eau", "Sur le master, tel quel : tête sous l'eau, étouffé avec goût, espace discret.",
         {{"ear", Diver}, {"ear_amount", 0.8f}, {"muffle", 0.28f}, {"distance", tDistance(3)}, {"src_depth", tDepth(3)}, {"lis_depth", tDepth(3.5f)},
          {"water_depth", tWaterDepth(10)}, {"bottom", Sand}, {"waves", tWaves(0.15f)}, {"space", Bay}, {"size", tSize(18)}, {"decay", tDecay(1.6f)},
          {"water_level", tLevel(-10)}, {"width", 0.38f}, {"wet", tLevel(-1)}}},
        {"MASTER", "Profond", "Sur le master : loin sous la surface, sombre et vaste.",
         {{"ear", Diver}, {"muffle", 0.5f}, {"distance", tDistance(12)}, {"src_depth", tDepth(40)}, {"lis_depth", tDepth(35)},
          {"water_depth", tWaterDepth(60)}, {"bottom", Mud}, {"waves", 0.0f}, {"space", OpenSea}, {"size", tSize(90)}, {"decay", tDecay(4.5f)},
          {"tone", 0.25f}, {"water_level", tLevel(-6)}, {"width", 0.3f}, {"wet", tLevel(-1)}}},
        {"MASTER", "Étouffé à fond", "MUFFLE poussé à l'abus : la pression dans les oreilles.",
         {{"ear", Cinema}, {"ear_amount", 1.0f}, {"muffle", 0.82f}, {"distance", tDistance(4)}, {"space", Bay}, {"decay", tDecay(2.2f)},
          {"water_level", tLevel(-5)}, {"width", 0.4f}, {"wet", tLevel(-3.5f)}}},
        // SCÈNE
        {"SCÈNE", "Plongeon", "Automatiser SUBMERGE de 0 à 100 % : la tête passe sous la surface.",
         {{"submerge", 0.0f}, {"ear", Diver}, {"muffle", 0.3f}, {"distance", tDistance(2)}, {"src_depth", tDepth(1)}, {"lis_depth", tDepth(1.5f)},
          {"water_depth", tWaterDepth(5)}, {"waves", tWaves(0.3f)}, {"space", Bay}, {"size", tSize(15)}, {"decay", tDecay(1.5f)}, {"water_level", tLevel(-9)}}},
        {"SCÈNE", "Nage près de la surface", "Juste sous les vagues : la surface bouge chaque rebond.",
         {{"ear", Diver}, {"distance", tDistance(1.5f)}, {"src_depth", tDepth(0.6f)}, {"lis_depth", tDepth(0.8f)}, {"water_depth", tWaterDepth(4)},
          {"sway", tSway(0.8f)}, {"waves", tWaves(0.45f)}, {"muffle", 0.2f}, {"space", Bay}, {"size", tSize(15)}, {"decay", tDecay(1.3f)}, {"water_level", tLevel(-10)}}},
        {"SCÈNE", "Sirène, voix proche", "Une voix sous l'eau qui reste intelligible (CLARITY).",
         {{"ear", Diver}, {"ear_amount", 0.7f}, {"clarity", 0.65f}, {"distance", tDistance(1.2f)}, {"src_depth", tDepth(3)}, {"lis_depth", tDepth(3)},
          {"water_depth", tWaterDepth(15)}, {"waves", tWaves(0.1f)}, {"space", Bay}, {"size", tSize(25)}, {"decay", tDecay(2.5f)},
          {"water_level", tLevel(-11)}, {"width", 0.4f}}},
        {"SCÈNE", "Sirène, chant lointain", "Un chant qui porte : trajets multiples, écho d'une falaise, queue sombre.",
         {{"ear", Diver}, {"clarity", 0.45f}, {"distance", tDistance(60)}, {"src_depth", tDepth(8)}, {"lis_depth", tDepth(4)}, {"water_depth", tWaterDepth(25)},
          {"waves", tWaves(0.25f)}, {"reflections", 0.6f}, {"echo_on", 1.0f}, {"echo_wall", Cliff}, {"echo_dist", tEcho(180)}, {"echo_fb", 0.35f},
          {"echo_level", tLevel(-12)}, {"space", OpenSea}, {"size", tSize(120)}, {"decay", tDecay(6)}, {"tone", 0.3f}, {"water_level", tLevel(-5)}}},
        {"SCÈNE", "Coquille géante", "Tout près, au fond, à 15 m : lourd, sourd, enveloppé.",
         {{"ear", Diver}, {"muffle", 0.35f}, {"distance", tDistance(2.5f)}, {"src_depth", tDepth(14.5f)}, {"lis_depth", tDepth(12)}, {"water_depth", tWaterDepth(15)},
          {"bottom", Sand}, {"space", Bay}, {"size", tSize(30)}, {"decay", tDecay(2.4f)}, {"water_level", tLevel(-8)}, {"eq_low", tBipolar(3, 12)}}},
        {"SCÈNE", "Fonds marins qui bougent", "La source dérive, la houle module chaque rebond.",
         {{"ear", Diver}, {"distance", tDistance(6)}, {"src_depth", tDepth(9)}, {"lis_depth", tDepth(6)}, {"water_depth", tWaterDepth(10)},
          {"bottom", Gravel}, {"sway", tSway(2.5f)}, {"waves", tWaves(0.5f)}, {"muffle", 0.25f}, {"space", Bay}, {"size", tSize(40)}, {"decay", tDecay(2.8f)},
          {"water_level", tLevel(-7)}}},
        // LIEU
        {"LIEU", "Récif", "Fond de corail rugueux : rebonds sombres et diffus.",
         {{"ear", Diver}, {"bottom", Reef}, {"water_depth", tWaterDepth(8)}, {"distance", tDistance(4)}, {"src_depth", tDepth(4)}, {"lis_depth", tDepth(5)},
          {"echo_on", 1.0f}, {"echo_wall", ReefWall}, {"echo_dist", tEcho(25)}, {"echo_fb", 0.15f}, {"echo_level", tLevel(-14)},
          {"space", Bay}, {"size", tSize(25)}, {"decay", tDecay(1.8f)}}},
        {"LIEU", "Grotte sous-marine", "Roche : résonances graves, écho court, longue queue sombre.",
         {{"ear", Diver}, {"bottom", Rock}, {"water_depth", tWaterDepth(20)}, {"distance", tDistance(5)}, {"src_depth", tDepth(10)}, {"lis_depth", tDepth(12)},
          {"waves", 0.0f}, {"echo_on", 1.0f}, {"echo_wall", Cliff}, {"echo_dist", tEcho(18)}, {"echo_fb", 0.45f}, {"echo_level", tLevel(-10)},
          {"space", Cave}, {"size", tSize(25)}, {"decay", tDecay(3.5f)}, {"tone", 0.3f}, {"water_level", tLevel(-4)}}},
        {"LIEU", "Épave", "Une coque d'acier qui chante autour de la source.",
         {{"ear", Diver}, {"bottom", Rock}, {"water_depth", tWaterDepth(22)}, {"distance", tDistance(3)}, {"src_depth", tDepth(18)}, {"lis_depth", tDepth(17)},
          {"waves", 0.0f}, {"echo_on", 1.0f}, {"echo_wall", Hull}, {"echo_dist", tEcho(9)}, {"echo_fb", 0.4f}, {"echo_level", tLevel(-11)},
          {"space", Wreck}, {"size", tSize(18)}, {"decay", tDecay(3)}, {"tone", 0.5f}, {"water_level", tLevel(-5)}}},
        {"LIEU", "Piscine", "Carrelage dur, peu de profondeur : flutter dense et brillant.",
         {{"ear", Diver}, {"bottom", Rock}, {"water_depth", tWaterDepth(2.2f)}, {"distance", tDistance(4)}, {"src_depth", tDepth(1)}, {"lis_depth", tDepth(1.2f)},
          {"waves", tWaves(0.03f)}, {"space", Pool}, {"size", tSize(12)}, {"decay", tDecay(2.6f)}, {"tone", 0.65f}, {"water_level", tLevel(-3)}}},
        {"LIEU", "Aquarium", "Un petit bac de verre, entendu à l'hydrophone.",
         {{"ear", Hydrophone}, {"bottom", Rock}, {"water_depth", tWaterDepth(1)}, {"distance", tDistance(0.6f)}, {"src_depth", tDepth(0.4f)}, {"lis_depth", tDepth(0.5f)},
          {"waves", 0.0f}, {"space", Tank}, {"size", tSize(2.5f)}, {"decay", tDecay(0.8f)}, {"tone", 0.7f}, {"water_level", tLevel(-6)}}},
        {"LIEU", "Sous la banquise", "Un plafond de glace lisse et un mur de glace au loin.",
         {{"ear", Diver}, {"water_depth", tWaterDepth(120)}, {"distance", tDistance(15)}, {"src_depth", tDepth(6)}, {"lis_depth", tDepth(5)}, {"waves", 0.0f},
          {"echo_on", 1.0f}, {"echo_wall", Ice}, {"echo_dist", tEcho(300)}, {"echo_fb", 0.5f}, {"echo_level", tLevel(-9)},
          {"space", OpenSea}, {"size", tSize(150)}, {"decay", tDecay(7)}, {"tone", 0.55f}, {"water_level", tLevel(-6)}}},
        {"LIEU", "Haute mer, 1 km", "Une source lointaine : trajets rasants, houle forte.",
         {{"ear", Diver}, {"distance", tDistance(1000)}, {"src_depth", tDepth(20)}, {"lis_depth", tDepth(10)}, {"water_depth", tWaterDepth(80)},
          {"waves", tWaves(0.8f)}, {"muffle", 0.3f}, {"space", OpenSea}, {"size", tSize(150)}, {"decay", tDecay(7)}, {"water_level", tLevel(-4)}}},
        {"LIEU", "Abysses, 4000 m", "La plaine abyssale : rien au-dessus, une queue immense et noire.",
         {{"ear", Diver}, {"bottom", Mud}, {"water_depth", tWaterDepth(4000)}, {"distance", tDistance(40)}, {"src_depth", tDepth(3950)}, {"lis_depth", tDepth(3930)},
          {"waves", 0.0f}, {"muffle", 0.45f}, {"space", Abyss}, {"size", tSize(200)}, {"decay", tDecay(12)}, {"tone", 0.15f}, {"water_level", tLevel(-3)}}},
        {"LIEU", "Fosse des Mariannes", "10 994 m : le point le plus profond des océans.",
         {{"ear", Diver}, {"bottom", Mud}, {"water_depth", tWaterDepth(10994)}, {"distance", tDistance(25)}, {"src_depth", tDepth(10980)}, {"lis_depth", tDepth(10960)},
          {"waves", 0.0f}, {"muffle", 0.55f}, {"echo_on", 1.0f}, {"echo_wall", Cliff}, {"echo_dist", tEcho(900)}, {"echo_fb", 0.3f}, {"echo_level", tLevel(-14)},
          {"space", Abyss}, {"size", tSize(200)}, {"decay", tDecay(16)}, {"tone", 0.1f}, {"water_level", tLevel(-3)}}},
        // ÉCOUTE
        {"ÉCOUTE", "Hydrophone documentaire", "Ce qu'enregistre un hydrophone : plein spectre, rien n'est étouffé.",
         {{"ear", Hydrophone}, {"ear_amount", 1.0f}, {"water_level", tLevel(-12)}}},
        {"ÉCOUTE", "Scaphandre", "Un casque de scaphandrier : une poche d'air qui résonne.",
         {{"ear", Helmet}, {"muffle", 0.2f}, {"water_level", tLevel(-9)}}},
        {"ÉCOUTE", "Cinéma classique", "La convention de film : passe-bas raide et résonant, bas chaleureux.",
         {{"ear", Cinema}, {"ear_amount", 1.0f}, {"waves", tWaves(0.3f)}, {"water_level", tLevel(-6)}}},
    };
    return list;
}

// The values of a preset for every parameter (defaults where it says nothing).
inline std::vector<float> valuesOf(const Preset& p) {
    const auto& specs = params::all();
    std::vector<float> v(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        v[i] = specs[i].defaultValue;
        for (const auto& [id, value] : p.values)
            if (id == specs[i].id) v[i] = value;
    }
    return v;
}

}  // namespace silt::presets
