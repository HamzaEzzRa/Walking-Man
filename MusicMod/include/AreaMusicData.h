#pragma once

#include <cstdint>
#include <cstring>

#include "GameData.h"

namespace AreaMusic
{
	struct AreaMusicChain
	{
		const char* songName = nullptr;
		uint32_t ranseqId = 0;
		uint32_t segmentId = 0;
		uint32_t trackId = 0;
		uint32_t sourceId = 0;
	};

	struct RegisterRequest
	{
		const MusicData* data = nullptr;
		long long sourceStartMs = 0;
		bool handled = false;
		bool success = false;
		bool metadataPatched = false;
		long long effectiveDurationMs = 0;
		long long effectiveSourceStartMs = 0;
	};

	struct PatchNativeOffsetRequest
	{
		const AreaMusicChain* chain = nullptr;
		long long sourceStartMs = 0;
		bool handled = false;
		bool success = false;
	};

	inline constexpr AreaMusicChain OverrideTarget{
		"Almost Nothing (No Beatbox)",
		800867414,
		165284959,
		57199966,
		1019908505
	};

	inline constexpr AreaMusicChain knownAreaMusicChains[] = {
		{"Don't Be So Serious",              124768037,  515599299,   864381405,   14330364},
		{"Bones",                            626513893,  924803744,   585844366,   443301537},
		{"Poznan",                           896125235,  569475449,   905168316,   112514829},
		{"Anything You Need",                608762905,  871006524,   353167231,   990235427},
		{"Easy Way Out",                     922675051,  136040502,   411554128,   573918731},
		{"I'm Leaving",                      504066054,  1004761732,  962966379,   10799056},
		{"Give Up",                          558666898,  233422843,   312215729,   409126050},
		{"Gosia",                            359904631,  316694681,   928172596,   1049365324},
		{"Without You",                      598222498,  352981518,   746624823,   234016975},
		{"Breathe In",                       821147021,  849466441,   318759929,   811953013},
		{"Because We Have To",               925185256,  667964839,   747024383,   44821658},
		{"St. Eriksplan",                    799932185,  520902395,   765370839,   894314852},
		{"Rolling Over",                     899700136,  992828349,   542864971,   271832813},
		{"Once in a Long, Long While...",    481140154,  80937257,    396340672,   534432074},
		{"The Machine",                      177160982,  209990408,   878567193,   931685189},
		{"Patience",                         356011358,  62893057,    704429786,   1065659883},
		{"Not Around",                       970173279,  943762408,   44283622,    428823264},
		{"Please Don't Stop (Chapter 1)",    524824844,  395048971,   533936954,   381534270},
		{"Tonight, tonight, tonight",        339126489,  1031914594,  1028497934,  819564709},
		{"Please Don't Stop (Chapter 2)",    508496364,  980712044,   438536405,   574698350},
		{"Half Asleep",                      1049968,    794164252,   240303610,   320869737},
		{"Waiting (10 Years)",               927076567,  445852071,   315451892,   1054849995},
		{"Nobody Else",                      440407878,  1046003322,  205774222,   300265341},
		{"Asylums For The Feeling",          1023121111, 22286178,    993579869,   208171504},
		{"Almost Nothing",                   1035153539, 190996573,   61205935,    772358996},
		{"BB's Theme",                       727898186,  161525664,   47642293,    554273859},
		{"Pale Yellow",                      624312543,  172520581,   372066990,   630551876},
		{"Goliath",                          129927267,  250138303,   144702745,   210092545},
		{"Control",                          370578840,  899176194,   529120711,   674482479},
		{"Other Me",                         881156219,  506330902,   260773589,   628634042},
		{"Fragile",                          209950558,  771416739,   359994144,   152808688},
		{"Ambient 1",                        174738457,  906105034,   997241961,   612428356},
		{"Ambient 4",                        663805971,  87624654,    1012293903,  378980528},
		{"Ambient 5",                        421722346,  113873444,   901357345,   491944249},
		{"Ambient 6",                        437838398,  397518684,   258488343,   459118830},
		{"Ambient 7",                        475318914,  321848901,   1057275913,  43133207},
		{"Almost Nothing (Instrumental)",    361567052,  828378002,   424219496,   303176799},
	};

	inline bool UsesCustomMediaOverride(const MusicData* data)
	{
		return data && data->customAreaTrack && data->customWemPath;
	}

	inline bool UsesInternalWwiseOverride(const MusicData* data)
	{
		return data
			&& data->customAreaTrack
			&& !data->customWemPath
			&& data->internalWwiseAreaTrack.sourceId != 0;
	}

	inline bool UsesOverride(const MusicData* data)
	{
		return UsesCustomMediaOverride(data) || UsesInternalWwiseOverride(data);
	}

	inline bool IsTemplateTrack(const MusicData* data)
	{
		return UsesOverride(data)
			|| (data && data->name && std::strcmp(data->name, OverrideTarget.songName) == 0);
	}

	inline const AreaMusicChain* LookupChainForSong(const char* songName)
	{
		if (!songName)
		{
			return nullptr;
		}

		if (std::strcmp(songName, OverrideTarget.songName) == 0)
		{
			return &OverrideTarget;
		}

		for (const AreaMusicChain& chain : knownAreaMusicChains)
		{
			if (chain.songName && std::strcmp(songName, chain.songName) == 0)
			{
				return &chain;
			}
		}
		return nullptr;
	}
}
