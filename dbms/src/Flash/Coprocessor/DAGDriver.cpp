#include <Core/QueryProcessingStage.h>
#include <DataStreams/BlockIO.h>
#include <DataStreams/IProfilingBlockInputStream.h>
#include <DataStreams/copyData.h>
#include <Flash/Coprocessor/DAGBlockOutputStream.h>
#include <Flash/Coprocessor/DAGDriver.h>
#include <Flash/Coprocessor/DAGQuerySource.h>
#include <Flash/Coprocessor/DAGStringConverter.h>
#include <Interpreters/Context.h>
#include <Interpreters/executeQuery.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Transaction/LockException.h>
#include <Storages/Transaction/RegionException.h>
#include <Storages/Transaction/TMTContext.h>
#include <pingcap/Exception.h>
#include <pingcap/kv/LockResolver.h>

namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int UNKNOWN_EXCEPTION;
} // namespace ErrorCodes

template <>
DAGDriver<false>::DAGDriver(Context & context_, const tipb::DAGRequest & dag_request_,
    const std::unordered_map<RegionID, RegionInfo> & regions_, UInt64 start_ts, UInt64 schema_ver, tipb::SelectResponse * dag_response_,
    bool internal_)
    : context(context_),
      dag_request(dag_request_),
      regions(regions_),
      dag_response(dag_response_),
      writer(nullptr),
      internal(internal_),
      log(&Logger::get("DAGDriver"))
{
    context.setSetting("read_tso", start_ts);
    if (schema_ver)
        // schema_ver being 0 means TiDB/TiSpark hasn't specified schema version.
        context.setSetting("schema_version", schema_ver);
    context.getTimezoneInfo().resetByDAGRequest(dag_request);
}

template <>
DAGDriver<true>::DAGDriver(Context & context_, const tipb::DAGRequest & dag_request_,
    const std::unordered_map<RegionID, RegionInfo> & regions_, UInt64 start_ts, UInt64 schema_ver,
    ::grpc::ServerWriter<::coprocessor::BatchResponse> * writer_, bool internal_)
    : context(context_), dag_request(dag_request_), regions(regions_), writer(writer_), internal(internal_), log(&Logger::get("DAGDriver"))
{
    context.setSetting("read_tso", start_ts);
    if (schema_ver)
        // schema_ver being 0 means TiDB/TiSpark hasn't specified schema version.
        context.setSetting("schema_version", schema_ver);
    context.getTimezoneInfo().resetByDAGRequest(dag_request);
}

template <bool batch>
void DAGDriver<batch>::execute()
try
{
    DAGContext dag_context(dag_request);
    context.setDAGContext(&dag_context);
    DAGQuerySource dag(context, regions, dag_request, writer, batch);

    BlockIO streams = executeQuery(dag, context, internal, QueryProcessingStage::Complete);
    if (!streams.in || streams.out)
        // Only query is allowed, so streams.in must not be null and streams.out must be null
        throw TiFlashException("DAG is not query.", Errors::Coprocessor::Internal);

    if constexpr (!batch)
    {
        bool collect_exec_summary = dag_request.has_collect_execution_summaries() && dag_request.collect_execution_summaries();
        BlockOutputStreamPtr dag_output_stream
            = std::make_shared<DAGBlockOutputStream>(dag_response, context.getSettings().dag_records_per_chunk, dag.getEncodeType(),
                dag.getResultFieldTypes(), streams.in->getHeader(), dag_context, collect_exec_summary, dag_request.has_root_executor());
        copyData(*streams.in, *dag_output_stream);
    }
    else
    {
        streams.in->readPrefix();
        while (streams.in->read())
            ;
        streams.in->readSuffix();
    }

    if (auto * p_stream = dynamic_cast<IProfilingBlockInputStream *>(streams.in.get()))
    {
        LOG_DEBUG(log,
            __PRETTY_FUNCTION__ << ": dag request with encode cost: " << p_stream->getProfileInfo().execution_time / (double)1000000000
                                << " seconds, produce " << p_stream->getProfileInfo().rows << " rows, " << p_stream->getProfileInfo().bytes
                                << " bytes.");

        if constexpr (!batch)
        {
            // Under some test cases, there may be dag response whose size is bigger than INT_MAX, and GRPC can not limit it.
            // Throw exception to prevent receiver from getting wrong response.
            if (p_stream->getProfileInfo().bytes > std::numeric_limits<int>::max())
                throw TiFlashException("DAG response is too big, please check config about region size or region merge scheduler",
                    Errors::Coprocessor::Internal);
        }
    }
}
catch (const RegionException & e)
{
    throw;
}
catch (const LockException & e)
{
    throw;
}
catch (const TiFlashException & e)
{
    LOG_ERROR(log, __PRETTY_FUNCTION__ << ": " << e.standardText() << "\n" << e.getStackTrace().toString());
    recordError(grpc::StatusCode::INTERNAL, e.standardText());
}
catch (const Exception & e)
{
    LOG_ERROR(log, __PRETTY_FUNCTION__ << ": DB Exception: " << e.message() << "\n" << e.getStackTrace().toString());
    recordError(e.code(), e.message());
}
catch (const pingcap::Exception & e)
{
    LOG_ERROR(log, __PRETTY_FUNCTION__ << ": KV Client Exception: " << e.message());
    recordError(e.code(), e.message());
}
catch (const std::exception & e)
{
    LOG_ERROR(log, __PRETTY_FUNCTION__ << ": std exception: " << e.what());
    recordError(ErrorCodes::UNKNOWN_EXCEPTION, e.what());
}
catch (...)
{
    LOG_ERROR(log, __PRETTY_FUNCTION__ << ": other exception");
    recordError(ErrorCodes::UNKNOWN_EXCEPTION, "other exception");
}

template <bool batch>
void DAGDriver<batch>::recordError(Int32 err_code, const String & err_msg)
{
    if constexpr (batch)
    {
        std::ignore = err_code;
        coprocessor::BatchResponse err_response;
        err_response.set_other_error(err_msg);
        writer->Write(err_response);
    }
    else
    {
        dag_response->Clear();
        tipb::Error * error = dag_response->mutable_error();
        error->set_code(err_code);
        error->set_msg(err_msg);
    }
}

template class DAGDriver<true>;
template class DAGDriver<false>;

} // namespace DB
