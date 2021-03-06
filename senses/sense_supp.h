/* (C)opyright, 2014-2015 Björn Ståhl
 * License: BSD 3-clause, see COPYING file in the senseye source repository.
 * Reference: senseye.arcan-fe.com
 *
 * Description: Interface for using the fdsense interface to senseye for
 * either cooperatively or through hijacking connect streaming transfers
 */

struct senseye_ch {
	void (*pump)(struct senseye_ch*);
	ssize_t (*data)(struct senseye_ch*, const void* buf, size_t ntw);
	off_t (*seek)(struct senseye_ch*, long long);
	void (*flush)(struct senseye_ch*);
	void (*queue)(struct senseye_ch*, arcan_event*);
	void (*close)(struct senseye_ch*);

/* little need to manipulate these manually,
 * but provided for more advanced use */
	struct rwstat_ch* in;
	struct senseye_priv* in_pr;
	int in_handle;
};

/*
 * refresh and dispatch can be overridden with a matching prototype
 * and will be invoked as part of the _pump() loop when necessary
 */
struct senseye_cont {
	bool (*refresh)(struct senseye_cont*, shmif_pixel* vidp, size_t w, size_t h);
	void (*dispatch)(struct senseye_cont*, arcan_event* ev);
	struct arcan_shmif_cont* (*context)(struct senseye_cont* c);

	void* tag;
	struct senseye_priv* priv;
};

/*
 * Initialization - open the connection to the arcan session that is
 * running senseye. The connection key can be NULL, then the usual
 * ARCAN_CONNPATH, ARCAN_ARGS mechanism will be used for finding the
 * server.
 *
 * will return a fdsense context in which new data channels can be opened
 */
bool senseye_connect(const char* key, FILE* logout,
	struct senseye_cont*, struct arg_arr**, enum SHMIF_FLAGS flags);

/*
 * treat as main-loop, implements the main control channel
 * semantics for connection with the UI (override the default
 * refresh and event handlers in the structure if needed)
 */
bool senseye_pump(struct senseye_cont*, bool block);

/*
 * Path is just a hint that will be used as a textual identifier
 * in the user-interface. Base is the initial dimensions of the
 * data transfers to the UI (should be a square power of 2)
 */
struct senseye_ch* senseye_open(struct senseye_cont* cont,
	const char* const ident, size_t base);
