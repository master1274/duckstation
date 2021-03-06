#include "gpu_backend.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "settings.h"
Log_SetChannel(GPUBackend);

std::unique_ptr<GPUBackend> g_gpu_backend;

GPUBackend::GPUBackend() = default;

GPUBackend::~GPUBackend() = default;

bool GPUBackend::Initialize()
{
  if (g_settings.gpu_use_thread)
    StartGPUThread();

  return true;
}

void GPUBackend::Reset()
{
  Sync();
  m_drawing_area = {};
}

void GPUBackend::UpdateSettings()
{
  Sync();

  if (m_use_gpu_thread != g_settings.gpu_use_thread)
  {
    if (!g_settings.gpu_use_thread)
      StopGPUThread();
    else
      StartGPUThread();
  }
}

void GPUBackend::Shutdown()
{
  StopGPUThread();
}

GPUBackendFillVRAMCommand* GPUBackend::NewFillVRAMCommand()
{
  GPUBackendFillVRAMCommand* cmd =
    static_cast<GPUBackendFillVRAMCommand*>(AllocateCommand(sizeof(GPUBackendFillVRAMCommand)));
  cmd->type = GPUBackendCommandType::FillVRAM;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendUpdateVRAMCommand* GPUBackend::NewUpdateVRAMCommand(u32 num_words)
{
  const u32 size = sizeof(GPUBackendUpdateVRAMCommand) + (num_words * sizeof(u16));
  GPUBackendUpdateVRAMCommand* cmd = static_cast<GPUBackendUpdateVRAMCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::UpdateVRAM;
  cmd->size = size;
  return cmd;
}

GPUBackendCopyVRAMCommand* GPUBackend::NewCopyVRAMCommand()
{
  GPUBackendCopyVRAMCommand* cmd =
    static_cast<GPUBackendCopyVRAMCommand*>(AllocateCommand(sizeof(GPUBackendCopyVRAMCommand)));
  cmd->type = GPUBackendCommandType::CopyVRAM;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendSetDrawingAreaCommand* GPUBackend::NewSetDrawingAreaCommand()
{
  GPUBackendSetDrawingAreaCommand* cmd =
    static_cast<GPUBackendSetDrawingAreaCommand*>(AllocateCommand(sizeof(GPUBackendSetDrawingAreaCommand)));
  cmd->type = GPUBackendCommandType::SetDrawingArea;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendDrawPolygonCommand* GPUBackend::NewDrawPolygonCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawPolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPolygonCommand::Vertex));
  GPUBackendDrawPolygonCommand* cmd = static_cast<GPUBackendDrawPolygonCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::DrawPolygon;
  cmd->size = size;
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

GPUBackendDrawRectangleCommand* GPUBackend::NewDrawRectangleCommand()
{
  GPUBackendDrawRectangleCommand* cmd =
    static_cast<GPUBackendDrawRectangleCommand*>(AllocateCommand(sizeof(GPUBackendDrawRectangleCommand)));
  cmd->type = GPUBackendCommandType::DrawRectangle;
  cmd->size = cmd->Size();
  return cmd;
}

GPUBackendDrawLineCommand* GPUBackend::NewDrawLineCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawLineCommand) + (num_vertices * sizeof(GPUBackendDrawLineCommand::Vertex));
  GPUBackendDrawLineCommand* cmd = static_cast<GPUBackendDrawLineCommand*>(AllocateCommand(size));
  cmd->type = GPUBackendCommandType::DrawLine;
  cmd->size = size;
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

void* GPUBackend::AllocateCommand(u32 size)
{
  for (;;)
  {
    u32 read_ptr = m_command_fifo_read_ptr.load();
    u32 write_ptr = m_command_fifo_write_ptr.load();
    if (read_ptr > write_ptr)
    {
      u32 available_size = read_ptr - write_ptr;
      while (available_size < (size + sizeof(GPUBackendCommandType)))
      {
        WakeGPUThread();
        read_ptr = m_command_fifo_read_ptr.load();
        available_size = (read_ptr > write_ptr) ? (read_ptr - write_ptr) : (COMMAND_QUEUE_SIZE - write_ptr);
      }
    }
    else
    {
      const u32 available_size = COMMAND_QUEUE_SIZE - write_ptr;
      if ((size + sizeof(GPUBackendCommand)) > available_size)
      {
        // allocate a dummy command to wrap the buffer around
        GPUBackendCommand* dummy_cmd = reinterpret_cast<GPUBackendCommand*>(&m_command_fifo_data[write_ptr]);
        dummy_cmd->type = GPUBackendCommandType::Wraparound;
        dummy_cmd->size = available_size;
        dummy_cmd->params.bits = 0;
        m_command_fifo_write_ptr.store(0);
        continue;
      }
    }

    return &m_command_fifo_data[write_ptr];
  }
}

u32 GPUBackend::GetPendingCommandSize() const
{
  const u32 read_ptr = m_command_fifo_read_ptr.load();
  const u32 write_ptr = m_command_fifo_write_ptr.load();
  return (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (COMMAND_QUEUE_SIZE - read_ptr + write_ptr);
}

void GPUBackend::PushCommand(GPUBackendCommand* cmd)
{
  if (!m_use_gpu_thread)
  {
    // single-thread mode
    if (cmd->type != GPUBackendCommandType::Sync)
      HandleCommand(cmd);
  }
  else
  {
    const u32 new_write_ptr = m_command_fifo_write_ptr.fetch_add(cmd->size) + cmd->size;
    DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
    if (GetPendingCommandSize() >= THRESHOLD_TO_WAKE_GPU)
      WakeGPUThread();
  }
}

void GPUBackend::WakeGPUThread()
{
  std::unique_lock<std::mutex> lock(m_sync_mutex);
  if (!m_gpu_thread_sleeping.load())
    return;

  m_wake_gpu_thread_cv.notify_one();
}

void GPUBackend::StartGPUThread()
{
  m_gpu_loop_done.store(false);
  m_use_gpu_thread = true;
  m_gpu_thread = std::thread(&GPUBackend::RunGPULoop, this);
  Log_InfoPrint("GPU thread started.");
}

void GPUBackend::StopGPUThread()
{
  if (!m_use_gpu_thread)
    return;

  m_gpu_loop_done.store(true);
  WakeGPUThread();
  m_gpu_thread.join();
  m_use_gpu_thread = false;
  Log_InfoPrint("GPU thread stopped.");
}

void GPUBackend::Sync()
{
  if (!m_use_gpu_thread)
    return;

  GPUBackendSyncCommand* cmd = static_cast<GPUBackendSyncCommand*>(AllocateCommand(sizeof(GPUBackendSyncCommand)));
  cmd->type = GPUBackendCommandType::Sync;
  cmd->size = sizeof(GPUBackendSyncCommand);
  PushCommand(cmd);
  WakeGPUThread();

  m_sync_event.Wait();
  m_sync_event.Reset();
}

void GPUBackend::RunGPULoop()
{
  for (;;)
  {
    u32 write_ptr = m_command_fifo_write_ptr.load();
    u32 read_ptr = m_command_fifo_read_ptr.load();
    if (read_ptr == write_ptr)
    {
      std::unique_lock<std::mutex> lock(m_sync_mutex);
      m_gpu_thread_sleeping.store(true);
      m_wake_gpu_thread_cv.wait(lock, [this]() { return m_gpu_loop_done.load() || GetPendingCommandSize() > 0; });
      m_gpu_thread_sleeping.store(false);

      if (m_gpu_loop_done.load())
        break;
      else
        continue;
    }

    if (write_ptr < read_ptr)
      write_ptr = COMMAND_QUEUE_SIZE;

    while (read_ptr < write_ptr)
    {
      const GPUBackendCommand* cmd = reinterpret_cast<const GPUBackendCommand*>(&m_command_fifo_data[read_ptr]);
      read_ptr += cmd->size;

      switch (cmd->type)
      {
        case GPUBackendCommandType::Wraparound:
        {
          DebugAssert(read_ptr == COMMAND_QUEUE_SIZE);
          write_ptr = m_command_fifo_write_ptr.load();
          read_ptr = 0;
        }
        break;

        case GPUBackendCommandType::Sync:
        {
          DebugAssert(read_ptr == write_ptr);
          m_sync_event.Signal();
        }
        break;

        default:
          HandleCommand(cmd);
          break;
      }
    }

    m_command_fifo_read_ptr.store(read_ptr);
  }
}

void GPUBackend::HandleCommand(const GPUBackendCommand* cmd)
{
  switch (cmd->type)
  {
    case GPUBackendCommandType::FillVRAM:
    {
      FlushRender();
      const GPUBackendFillVRAMCommand* ccmd = static_cast<const GPUBackendFillVRAMCommand*>(cmd);
      FillVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
               ccmd->color, ccmd->params);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      FlushRender();
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      UpdateVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
                 ccmd->data, ccmd->params);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      FlushRender();
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      CopyVRAM(ZeroExtend32(ccmd->src_x), ZeroExtend32(ccmd->src_y), ZeroExtend32(ccmd->dst_x),
               ZeroExtend32(ccmd->dst_y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height), ccmd->params);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      FlushRender();
      m_drawing_area = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd)->new_area;
      DrawingAreaChanged();
    }
    break;

    case GPUBackendCommandType::DrawPolygon:
    {
      DrawPolygon(static_cast<const GPUBackendDrawPolygonCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawRectangle:
    {
      DrawRectangle(static_cast<const GPUBackendDrawRectangleCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawLine:
    {
      DrawLine(static_cast<const GPUBackendDrawLineCommand*>(cmd));
    }
    break;

    default:
      break;
  }
}
