#pragma once

#include "stdint.h"

namespace Animations
{
	struct Animation;
}

namespace Modules
{
    enum AnimationEvent
    {
		AnimationEvent_None = 0,
        AnimationEvent_PickUp,
        AnimationEvent_Error,
        AnimationEvent_LowBattery,
        AnimationEvent_ChargingStart,
        AnimationEvent_ChargingDone,
        AnimationEvent_ChargingError,
        // Etc...
        AnimationEvent_Count
    };

	/// <summary>
	/// Manages a set of running animations, talking to the LED controller
	/// to tell it what LEDs must have what intensity at what time.
	/// </summary>
	namespace AnimController
	{
		void stopAtIndex(int animIndex);
		void removeAtIndex(int animIndex);
		void update();
		void update(int ms);

		void init();
		void stop();
		void play(AnimationEvent evt);
		void play(const Animations::Animation* anim);
		void stop(const Animations::Animation* anim);
		void stopAll();
	}
}
