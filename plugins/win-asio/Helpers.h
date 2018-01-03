/*Potentially convienient functions*/

/*
https://www.geeksforgeeks.org/count-set-bits-in-an-integer/
*/
/*
channel_count returns number of selected channels with a few formats, an int as a bitmask, 
a std::vector<bool> as a bitmask and two integer values as a range
*/
unsigned int channel_count(int n)
{
	unsigned int count = 0;
	while (n)
	{
		n &= (n - 1);
		count++;
	}
	return count;
}

unsigned int channel_count(std::vector<bool> bitmask) {
	int s = bitmask.size();
	unsigned int count = 0;
	for (int i = 0; i < s; i++) {
		if (bitmask[i]) {
			count++;
		}
	}
	return count;
}

unsigned int channel_count(int ch_range_1, int ch_range_2){
	int diff = abs(ch_range_1 - ch_range_2);
	return diff + 1;
}

/*valid_channel_configuration returns whether a number of selected channels fits within a specified speaker configuration*/
bool valid_channel_configuration(int channel_1, int channel_2, int speaker_count) {
	return ( channel_count(channel_1, channel_2) <= speaker_count ) && ( channel_1 >= 0 && channel_2 >= 0 );
}

bool valid_channel_configuration(int bitmask, int speaker_count) {
	return channel_count(bitmask) <= speaker_count;
}

bool valid_channel_configuration(std::vector<bool> bitmask, int speaker_count) {
	return channel_count(bitmask) <= speaker_count;
}
