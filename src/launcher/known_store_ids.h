#pragma once
#include <string_view>
#include <utility>
// Package -> Meta store app id of the tested titles: imported APKs link to their
// store listing without a lookup (store_link.h).
namespace venture::launcher {
inline constexpr std::pair<std::string_view, std::string_view> kKnownStoreIds[] = {
    {"com.RealitiesIO.puzzlingPlaces", "3931148300302917"},  // Puzzling Places
    {"com.gameboomvr.CookingSimulatorVR", "4428409203954512"},  // Cooking Simulator VR
    {"com.beatgames.beatsaber", "2448060205267927"},  // Beat Saber
    {"com.commutergames.vspeedwayalpha", "3927222113966481"},  // V-Speedway
    {"com.resolutiongames.baitsantacruz", "2082595615120854"},  // Bait!
    {"com.resolutiongames.ignis", "2307085352735834"},  // Blaston
    {"games.b4t.epicrollercoasters.oculus", "2299465166734471"},  // Epic Roller Coasters
    {"com.CactusVRStudios.CactusCowboyPlantsAtWar", "5569487753065341"},  // Cactus Cowboy - Plants At War
    {"com.magnopus.ElixirStore", "3793077684043441"},  // Elixir
    {"com.ILMxLAB.VaderImmortal.ep1", "2108775495884888"},  // Vader Immortal: Episode I
    {"com.camouflaj.manta", "3551691271620960"},  // Batman: Arkham Shadow
    {"com.Sanzaru.Wrath2", "2603836099654226"},  // Asgard's Wrath 2
    {"com.ILMxLAB.VaderImmortal.ep2", "2223615721048141"},  // Vader Immortal: Episode II
    {"com.ILMxLAB.VaderImmortal.ep3", "2426206484098337"},  // Vader Immortal: Episode III
    {"com.Armature.VR4", "2637179839719680"},  // Resident Evil 4
    {"com.SDI.TWD", "2897337400373711"},  // The Walking Dead: Saints & Sinners
    {"com.Oculus.FirstContact", "2188021891257542"},  // Oculus First Contact
    {"com.owlchemylabs.jobsimulator", "3235570703151406"},  // Job Simulator
    {"com.Ubisoft.ACNexusVR", "5812519008825194"},  // Assassin's Creed Nexus VR
    {"com.polyarc.MossGame", "1654565391314903"},  // Moss
    {"com.polyarc.Moss2", "4395292760584049"},  // Moss: Book II
    {"com.resolutiongames.demeo", "3634830803298285"},  // Demeo
};
}  // namespace venture::launcher
