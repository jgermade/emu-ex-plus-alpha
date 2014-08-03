#pragma once

/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#include <imagine/input/Input.hh>
#include <imagine/base/Window.hh>

namespace Input
{

template <class Range>
struct AxisKeyEmu
{
	Range lowLimit = 0, highLimit = 0;
	Key lowKey = 0, highKey = 0;
	int8 state = 0;

	constexpr AxisKeyEmu() {}
	constexpr AxisKeyEmu(Range lowLimit, Range highLimit, Key lowKey, Key highKey):
		lowLimit(lowLimit), highLimit(highLimit), lowKey(lowKey), highKey(highKey) {}

	bool update(Range pos, Key &released, Key &pushed)
	{
		int8 newState = (pos <= lowLimit) ? -1 :
			(pos >= highLimit) ? 1 :
			0;
		if(newState != state)
		{
			released = (state > 0) ? highKey : (state < 0) ? lowKey : 0;
			pushed = (newState > 0) ? highKey : (newState < 0) ? lowKey : 0;
			state = newState;
			return true;
		}
		return false;
	}

	bool dispatch(Range pos, uint id, uint map, const Device &dev, Base::Window &win)
	{
		Key releasedKey, pushedKey;
		if(!update(pos, releasedKey, pushedKey))
		{
			return false; // no change
		}
		if(releasedKey)
		{
			cancelKeyRepeatTimer();
			win.dispatchInputEvent(Event(id, map, releasedKey, RELEASED, 0, 0, &dev));
		}
		if(pushedKey)
		{
			Event event{id, map, pushedKey, PUSHED, 0, 0, &dev};
			startKeyRepeatTimer(event);
			win.dispatchInputEvent(event);
		}
		return true;
	}
};

}
