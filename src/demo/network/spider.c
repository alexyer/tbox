/* //////////////////////////////////////////////////////////////////////////////////////
 * includes
 */
#include "../demo.h"

/* //////////////////////////////////////////////////////////////////////////////////////
 * macros
 */ 

// the spider url maxn
#define TB_DEMO_SPIDER_URL_MAXN 		(4096)

// the spider task maxn
#define TB_DEMO_SPIDER_TASK_MAXN 		(100)

// the spider filter maxn
#define TB_DEMO_SPIDER_FILTER_MAXN 		(100000)

// the spider user agent
#define TB_DEMO_SPIDER_USER_AGENT 		"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_9_2) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/34.0.1847.137 Safari/537.36"

/* //////////////////////////////////////////////////////////////////////////////////////
 * types
 */ 

// the demo spider type
typedef struct __tb_demo_spider_t
{
	// the pool
	tb_handle_t 				pool;

	// the lock
	tb_spinlock_t 				lock;

	// the root
	tb_char_t 					root[256];

	// the filter
	tb_bloom_filter_t* 			filter;

}tb_demo_spider_t;

// the demo spider task type
typedef struct __tb_demo_spider_task_t
{
	// the pool
	tb_demo_spider_t* 			spider;

	// the iurl
	tb_char_t 					iurl[TB_DEMO_SPIDER_URL_MAXN];

	// the ourl
	tb_char_t 					ourl[TB_DEMO_SPIDER_URL_MAXN];

}tb_demo_spider_task_t;

/* //////////////////////////////////////////////////////////////////////////////////////
 * declaration
 */ 
static tb_void_t tb_demo_spider_task_exit(tb_demo_spider_task_t* task);
static tb_bool_t tb_demo_spider_task_done(tb_demo_spider_t* spider, tb_char_t const* url, tb_bool_t html, tb_bool_t* full);

/* //////////////////////////////////////////////////////////////////////////////////////
 * implementation
 */ 
#if defined(TB_CONFIG_MODULE_HAVE_CHARSET) \
	&& defined(TB_CONFIG_MODULE_HAVE_ASIO) \
	&& defined(TB_CONFIG_MODULE_HAVE_XML)
static tb_basic_stream_t* tb_demo_spider_parser_open_html(tb_char_t const* url)
{
	// check
	tb_assert_and_check_return_val(url, tb_null);

	// done
	tb_bool_t 			ok = tb_false;
	tb_basic_stream_t* 	stream = tb_null;
	do
	{
		// html?
		if (!tb_strstr(url, "html")) break;

		// init stream
		stream = tb_basic_stream_init_from_url(url);
		tb_assert_and_check_break(stream);

		// open stream
		if (!tb_basic_stream_open(stream)) break;

		// the stream size
		tb_hong_t size = tb_stream_size(stream);
		tb_check_break(size);

		// prefetch some data
		tb_byte_t* 	data = tb_null;
		tb_size_t 	need = tb_min((tb_size_t)size, 256);
		if (!tb_basic_stream_need(stream, &data, need)) break;

		// is html?
		ok = tb_strnistr((tb_char_t const*)data, need, "<html>")? tb_true : tb_false;

	} while (0);

	// failed?
	if (!ok) 
	{
		// exit stream
		if (stream) tb_basic_stream_exit(stream);
		stream = tb_null;
	}

	// ok?
	return stream;
}
static tb_size_t tb_demo_spider_parser_get_url(tb_handle_t reader, tb_char_t* data, tb_size_t maxn, tb_bool_t* html)
{
	// check
	tb_assert_and_check_return_val(reader && data && maxn && html, tb_false);

	// walk
	tb_size_t ok = 0;
	tb_size_t event = TB_XML_READER_EVENT_NONE;
	while (!ok && (event = tb_xml_reader_next(reader)))
	{
		switch (event)
		{
		case TB_XML_READER_EVENT_ELEMENT_EMPTY: 
		case TB_XML_READER_EVENT_ELEMENT_BEG: 
			{
				// the element name
				tb_char_t const* name = tb_xml_reader_element(reader);
				tb_check_break(name);

				// <a href="" />? 
				// <link href="" /> 
				// <img src="" />? 
				// <script src="" />? 
				// <source src="" />? 
				// <frame src="" />? 
				if ( 	!tb_stricmp(name, "a")
					|| 	!tb_stricmp(name, "link")
					|| 	!tb_stricmp(name, "img")
					|| 	!tb_stricmp(name, "frame")
					|| 	!tb_stricmp(name, "source"))
				{
					// walk attributes
					tb_xml_node_t* attr = (tb_xml_node_t*)tb_xml_reader_attributes(reader);	
					for (; attr; attr = attr->next)
					{
						// href or src?
						if ( 	tb_scoped_string_size(&attr->data) > 8
							&& 	( 	!tb_scoped_string_cstricmp(&attr->name, "href")
								|| 	!tb_scoped_string_cstricmp(&attr->name, "src"))
							&& 	( 	!tb_scoped_string_cstrnicmp(&attr->data, "http://", 7)
								|| 	!tb_scoped_string_cstrnicmp(&attr->data, "https://", 8)))
						{
							// copy
							tb_strlcpy(data, tb_scoped_string_cstr(&attr->data), maxn);

							// ok
							ok = tb_scoped_string_size(&attr->data);

							// no html?
							if (!tb_stricmp(name, "img") || !tb_stricmp(name, "source"))
								*html = tb_false;
							else if ( 	ok > 4
									&& 	( 	!tb_stricmp(data + ok - 4, ".css")
										|| 	!tb_stricmp(data + ok - 4, ".png")
										|| 	!tb_stricmp(data + ok - 4, ".jpg")
										|| 	!tb_stricmp(data + ok - 4, ".gif")
										|| 	!tb_stricmp(data + ok - 4, ".rar")
										|| 	!tb_stricmp(data + ok - 4, ".zip")))
							{
								 *html = tb_false;
							}
							else if ( 	ok > 3
									&& 	( 	!tb_stricmp(data + ok - 4, ".js")
										|| 	!tb_stricmp(data + ok - 4, ".gz")))
							{
								 *html = tb_false;
							}
						}
					}
				}
			}
			break;
		default:
			break;
		}
	}

	// ok?
	return ok;
}
static tb_void_t tb_demo_spider_parser_done(tb_cpointer_t priv)
{
	// check
	tb_demo_spider_task_t* task = (tb_demo_spider_task_t*)priv;
	tb_assert_and_check_return(task);

	// init stream
	tb_basic_stream_t* stream = tb_demo_spider_parser_open_html(task->ourl);
	if (stream)
	{
		// init reader
		tb_handle_t reader = tb_xml_reader_init(stream);
		if (reader)
		{
			// parse url
			tb_char_t data[TB_DEMO_SPIDER_URL_MAXN] = {0};
			tb_bool_t html = tb_true;
			while (tb_demo_spider_parser_get_url(reader, data, sizeof(data) - 1, &html))
			{
				// trace
				tb_trace_d("parser: done: %s => %s", task->iurl, data);

				// done
				tb_bool_t full = tb_false;
				if (!tb_demo_spider_task_done(task->spider, data, html, &full) && full) break;

				// reset html
				html = tb_true;
			}

			// exit reader
			tb_xml_reader_exit(reader);
		}

		// exit stream
		tb_basic_stream_exit(stream);
	}
}
static tb_void_t tb_demo_spider_parser_exit(tb_cpointer_t priv)
{
	// check
	tb_demo_spider_task_t* task = (tb_demo_spider_task_t*)priv;
	tb_assert_and_check_return(task);

	// exit task
	tb_demo_spider_task_exit(task);
}
static tb_bool_t tb_demo_spider_make_ourl(tb_demo_spider_t* spider, tb_char_t const* url, tb_char_t* data, tb_size_t maxn, tb_bool_t html)
{
	// check
	tb_assert_and_check_return_val(spider && url && data && maxn, tb_false);

	// skip protocol
	tb_char_t* p = (tb_char_t*)url;
	tb_char_t* e = (tb_char_t*)url + tb_strlen(url);
	if (!tb_strnicmp(p, "http://", 7)) p += 7;
	else if (!tb_strnicmp(p, "https://", 8)) p += 8;
	tb_assert_and_check_return_val(p < e, tb_false);

	// make md5
	tb_byte_t md5_data[16];
	tb_size_t md5_size = tb_md5_encode((tb_byte_t const*)p, e - p, md5_data, 16);
	tb_assert_and_check_return_val(md5_size == 16, tb_false);

	// append root
	p = data;
	e = data + maxn;
	p += tb_snprintf(p, e - p, "%s/%s/", spider->root, html? "html" : "other");

	// append md5
	tb_size_t i = 0;
	for (i = 0; i < 16 && p < e; ++i) p += tb_snprintf(p, e - p, "%02X", md5_data[i]);
	tb_assert_and_check_return_val(p < e, tb_false);

	// end
	*p = '\0';

	// trace
	tb_trace_d("make: %s => %s", url, data);

	// ok?
	return i == 16? tb_true : tb_false;
}
static tb_void_t tb_demo_spider_task_exit(tb_demo_spider_task_t* task)
{
	// check
	tb_assert_and_check_return(task);

	// the spider 
	tb_demo_spider_t* spider = task->spider;
	tb_assert_and_check_return(spider);

	// enter
	tb_spinlock_enter(&spider->lock);

	// trace
	tb_trace_d("task: size: %lu, exit: %s", spider->pool? tb_fixed_pool_size(spider->pool) : 0, task->iurl);

	// exit task
	if (spider->pool) tb_fixed_pool_free(spider->pool, task);

	// leave
	tb_spinlock_leave(&spider->lock);
}
static tb_bool_t tb_demo_spider_task_save(tb_size_t state, tb_hize_t offset, tb_hong_t size, tb_hize_t save, tb_size_t rate, tb_pointer_t priv)
{
	// check
	tb_demo_spider_task_t* task = (tb_demo_spider_task_t*)priv;
	tb_assert_and_check_return_val(task, tb_false);

	// percent
	tb_size_t percent = 0;
	if (size > 0) percent = (offset * 100) / size;
	else if (state == TB_STATE_OK) percent = 100;

	// trace
	tb_trace_d("save[%s]: %llu, rate: %lu bytes/s, percent: %lu%%, state: %s", task->iurl, save, rate, percent, tb_state_cstr(state));

	// ok? continue it
	tb_bool_t ok = tb_false;
	if (state == TB_STATE_OK) ok = tb_true;
	// closed?
	else if (state == TB_STATE_CLOSED)
	{
		// trace
		tb_trace_i("task: done: %s: ok", task->iurl);

		// post parser task
		tb_thread_pool_task_post(tb_thread_pool(), "parser_task", tb_demo_spider_parser_done, tb_demo_spider_parser_exit, task, tb_false);
	}
	// failed or killed?
	else 
	{
		// trace
		tb_trace_e("task: done: %s: no", task->iurl);

		// exit task
		tb_demo_spider_task_exit(task);
	}

	// break or continue?
	return ok;
}
static tb_bool_t tb_demo_spider_task_ctrl(tb_stream_t* istream, tb_stream_t* ostream, tb_pointer_t priv)
{
	// check
	tb_assert_and_check_return_val(istream && ostream, tb_false);
	tb_assert_and_check_return_val(tb_stream_type(istream) == TB_STREAM_TYPE_HTTP, tb_false);

	// the url
	tb_char_t const* url = tb_null;
	if (!tb_stream_ctrl(istream, TB_STREAM_CTRL_GET_URL, &url)) return tb_false;

	// trace
	tb_trace_d("ctrl: %s: ..", url);

#ifdef TB_CONFIG_MODULE_HAVE_ZIP
	// need gzip
	if (!tb_stream_ctrl(istream, TB_STREAM_CTRL_HTTP_SET_HEAD, "Accept-Encoding", "gzip,deflate")) return tb_false;

	// auto unzip
	if (!tb_stream_ctrl(istream, TB_STREAM_CTRL_HTTP_SET_AUTO_UNZIP, 1)) return tb_false;
#endif

	// user agent
	if (!tb_stream_ctrl(istream, TB_STREAM_CTRL_HTTP_SET_HEAD, "User-Agent", TB_DEMO_SPIDER_USER_AGENT)) return tb_false;

	// ok
	return tb_true;
}
static tb_bool_t tb_demo_spider_task_done(tb_demo_spider_t* spider, tb_char_t const* url, tb_bool_t html, tb_bool_t* full)
{
	// check
	tb_assert_and_check_return_val(spider && url, tb_false);

	// enter
	tb_spinlock_enter(&spider->lock);

	// done
	tb_bool_t 				ok = tb_false;
	tb_size_t 				size = 0;
	tb_demo_spider_task_t* 	task = tb_null;
	tb_bool_t 				repeat = tb_false;
	do
	{
		// check
		tb_assert_and_check_break(spider->filter && spider->pool);

		// the task count
		size = tb_fixed_pool_size(spider->pool);
		
		// have been done already?
		if (!tb_bloom_filter_set(spider->filter, url)) 
		{
			// trace
			tb_trace_d("task: size: %lu, done: %s: repeat", size, url);
			ok = tb_true;
			repeat = tb_true;
			break;
		}

		// trace
		tb_trace_d("task: size: %lu, done: %s: ..", size, url);

		// full?
		tb_check_break(size < TB_DEMO_SPIDER_TASK_MAXN);

		// make task
		task = (tb_demo_spider_task_t*)tb_fixed_pool_malloc0(spider->pool);
		tb_assert_and_check_break(task);

		// init task
		task->spider = spider;
		tb_strlcpy(task->iurl, url, sizeof(task->iurl) - 1);
		if (!tb_demo_spider_make_ourl(spider, url, task->ourl, sizeof(task->ourl) - 1, html)) break;

		// ok 
		ok = tb_true;

	} while (0);

	// failed?
	if (!ok)
	{
		// exit task
		if (task) tb_demo_spider_task_exit(task);
		task = tb_null;
	}

	// leave
	tb_spinlock_leave(&spider->lock);

	// ok? done task
	if (ok && !repeat) ok = task? tb_transfer_pool_done(tb_transfer_pool(), url, task->ourl, 0, tb_demo_spider_task_save, tb_demo_spider_task_ctrl, task) : tb_false;

	// failed?
	if (!ok && size < TB_DEMO_SPIDER_TASK_MAXN)
	{
		// trace
		tb_trace_e("task: size: %lu, done: %s: no", size, url);
	}

	// save full
	if (full) *full = size < TB_DEMO_SPIDER_TASK_MAXN? tb_false : tb_true;

	// ok?
	return ok;
}
static tb_bool_t tb_demo_spider_init(tb_demo_spider_t* spider, tb_char_t const* root)
{
	// check
	tb_assert_and_check_return_val(spider, tb_false);

	// done
	tb_bool_t ok = tb_false;
	do
	{
		// init root
		if (root) tb_strlcpy(spider->root, root, sizeof(spider->root) - 1);
		else 
		{
			// the temporary root
			tb_directory_temp(spider->root, sizeof(spider->root) - 1);

			// append spider
			tb_strcat(spider->root, "/spider");
		}
		tb_trace_d("root: %s", spider->root);

		// strip root tail: '/' or '\\'
		tb_size_t size = tb_strlen(spider->root);
		if (size && (spider->root[size - 1] == '/' || spider->root[size - 1] == '\\')) spider->root[size - 1] = '\0';

		// init lock
		if (!tb_spinlock_init(&spider->lock)) break;

		// init pool
		spider->pool = tb_fixed_pool_init(TB_DEMO_SPIDER_TASK_MAXN, sizeof(tb_demo_spider_task_t), 0);
		tb_assert_and_check_break(spider->pool);

		// init filter
		spider->filter = tb_bloom_filter_init(TB_BLOOM_FILTER_PROBABILITY_0_001, 3, TB_DEMO_SPIDER_FILTER_MAXN, tb_item_func_str(tb_true, tb_null));
		tb_assert_and_check_break(spider->filter);

		// register lock profiler
#ifdef TB_LOCK_PROFILER_ENABLE
		tb_lock_profiler_register(tb_lock_profiler(), (tb_pointer_t)&spider->lock, "spider");
#endif

		// ok
		ok = tb_true;

	} while (0);

	// ok?
	return ok;
}
static tb_void_t tb_demo_spider_exit(tb_demo_spider_t* spider)
{
	// check
	tb_assert_and_check_return(spider);

	// trace
	tb_trace_d("exit: ..");

	// kill all parser tasks
	tb_thread_pool_task_kill_all(tb_thread_pool());

	// kill all transfer tasks
	tb_transfer_pool_kill(tb_transfer_pool());

	// wait all parser tasks exiting
	tb_thread_pool_task_wait_all(tb_thread_pool(), -1);

	// wait all transfer tasks exiting
	while (tb_transfer_pool_size(tb_transfer_pool()))
	{
		// wait some time
		tb_msleep(500);
	}

	// enter
	tb_spinlock_enter(&spider->lock);

	// exit filter
	if (spider->filter) tb_bloom_filter_exit(spider->filter);
	spider->filter = tb_null;

	// exit pool
	if (spider->pool) tb_fixed_pool_exit(spider->pool);
	spider->pool = tb_null;

	// leave
	tb_spinlock_leave(&spider->lock);

	// exit lock
	tb_spinlock_exit(&spider->lock);

	// trace
	tb_trace_d("exit: ok");
}

/* //////////////////////////////////////////////////////////////////////////////////////
 * main
 */ 
tb_int_t tb_demo_network_spider_main(tb_int_t argc, tb_char_t** argv)
{
	// done
	tb_demo_spider_t spider = {0};
	do
	{
		// init home
		tb_char_t const* home = argv[1]? argv[1] : tb_null;
		tb_trace_d("home: %s", home);

		// init spider
		if (!tb_demo_spider_init(&spider, argv[2])) break;

		// done the home task if exists
		if (home) tb_demo_spider_task_done(&spider, home, tb_true, tb_null);

		// wait 
		getchar();

	} while (0);

	// exit spider
	tb_demo_spider_exit(&spider);

	// end
	return 0;
}
#else
tb_int_t tb_demo_network_spider_main(tb_int_t argc, tb_char_t** argv)
{
	return 0;
}
#endif