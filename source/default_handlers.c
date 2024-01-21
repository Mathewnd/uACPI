#include <uacpi/internal/opregion.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/kernel_api.h>
#include <uacpi/internal/utilities.h>
#include <uacpi/uacpi.h>

struct pci_region_ctx {
    uacpi_pci_address address;
};

static uacpi_bool is_node_pci_root(uacpi_namespace_node *node)
{
    uacpi_status st;
    uacpi_bool ret = UACPI_FALSE;
    uacpi_char *id = UACPI_NULL;
    uacpi_pnp_id_list id_list = { 0 };

    st = uacpi_eval_hid(node, &id);
    if (st == UACPI_STATUS_OK && uacpi_is_pci_root_bridge(id)) {
        ret = UACPI_TRUE;
        goto out;
    }

    st = uacpi_eval_cid(node, &id_list);
    if (st == UACPI_STATUS_OK) {
        uacpi_size i;

        for (i = 0; i < id_list.num_entries; ++i) {
            if (uacpi_is_pci_root_bridge(id_list.ids[i])) {
                ret = UACPI_TRUE;
                goto out;
            }
        }
    }

out:
    uacpi_kernel_free(id);
    uacpi_release_pnp_id_list(&id_list);
    return ret;
}

static uacpi_namespace_node *find_pci_root(uacpi_namespace_node *node)
{
    uacpi_namespace_node *parent = node->parent;

    while (parent != uacpi_namespace_root()) {
        if (is_node_pci_root(parent)) {
            uacpi_trace(
                "found a PCI root node %.4s controlling region %.4s\n",
                parent->name.text, node->name.text
            );
            return parent;
        }

        parent = parent->parent;
    }

    uacpi_trace_region_error(
        node, "unable to find PCI root controlling",
        UACPI_STATUS_NOT_FOUND
    );
    return node;
}

static uacpi_status pci_region_attach(uacpi_region_attach_data *data)
{
    struct pci_region_ctx *ctx;
    uacpi_operation_region *op_region;
    uacpi_namespace_node *node, *pci_root;
    uacpi_object *obj;
    uacpi_status ret = UACPI_STATUS_OK;

    ctx = uacpi_kernel_calloc(1, sizeof(*ctx));
    if (ctx == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    node = data->region_node;
    op_region = uacpi_namespace_node_get_object(node)->op_region;

    pci_root = find_pci_root(node);

    /*
     * Find the actual device object that is supposed to be controlling
     * this operation region.
     */
    while (node) {
        obj = uacpi_namespace_node_get_object(node);
        if (obj && obj->type == UACPI_OBJECT_DEVICE)
            break;

        node = node->parent;
    }

    if (uacpi_unlikely(node == UACPI_NULL)) {
        ret = UACPI_STATUS_NOT_FOUND;
        uacpi_trace_region_error(
            node, "unable to find device responsible for", ret
        );
        uacpi_kernel_free(ctx);
        return ret;
    }

    ret = uacpi_eval_typed(
        node, "_ADR", UACPI_NULL,
        UACPI_OBJECT_INTEGER_BIT, &obj
    );
    if (ret == UACPI_STATUS_OK) {
        ctx->address.function = (obj->integer >> 0)  & 0xFF;
        ctx->address.device   = (obj->integer >> 16) & 0xFF;
    }

    ret = uacpi_eval_typed(
        pci_root, "_SEG", UACPI_NULL,
        UACPI_OBJECT_INTEGER_BIT, &obj
    );
    if (ret == UACPI_STATUS_OK)
        ctx->address.segment = obj->integer;

    ret = uacpi_eval_typed(
        pci_root, "_BBN", UACPI_NULL,
        UACPI_OBJECT_INTEGER_BIT, &obj
    );
    if (ret == UACPI_STATUS_OK)
        ctx->address.bus = obj->integer;

    uacpi_trace(
        "detected PCI device %.4s@%04X:%02X:%02X:%01X\n",
        node->name.text, ctx->address.segment, ctx->address.bus,
        ctx->address.device, ctx->address.function
    );

    data->out_region_context = ctx;
    return UACPI_STATUS_OK;
}

static uacpi_status pci_region_detach(uacpi_region_detach_data *data)
{
    struct pci_region_ctx *ctx = data->region_context;

    uacpi_kernel_free(ctx);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_region_do_rw(
    uacpi_region_op op, uacpi_region_rw_data *data
)
{
    struct pci_region_ctx *ctx = data->region_context;
    uacpi_u8 width;
    uacpi_size offset;

    offset = data->offset;
    width = data->byte_width;

    return op == UACPI_REGION_OP_READ ?
           uacpi_kernel_pci_read(&ctx->address, offset, width, &data->value) :
           uacpi_kernel_pci_write(&ctx->address, offset, width, data->value);
}

static uacpi_status handle_pci_region(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op) {
    case UACPI_REGION_OP_ATTACH:
        return pci_region_attach(op_data);
    case UACPI_REGION_OP_DETACH:
        return pci_region_detach(op_data);
    case UACPI_REGION_OP_READ:
    case UACPI_REGION_OP_WRITE:
        return pci_region_do_rw(op, op_data);
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
}

struct memory_region_ctx {
    uacpi_phys_addr phys;
    uacpi_u8 *virt;
    uacpi_size size;
};

static uacpi_status memory_region_attach(uacpi_region_attach_data *data)
{
    struct memory_region_ctx *ctx;
    uacpi_operation_region *op_region;
    uacpi_status ret = UACPI_STATUS_OK;

    ctx = uacpi_kernel_alloc(sizeof(*ctx));
    if (ctx == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    op_region = uacpi_namespace_node_get_object(data->region_node)->op_region;
    ctx->size = op_region->length;

    // FIXME: this really shouldn't try to map everything at once
    ctx->phys = op_region->offset;
    ctx->virt = uacpi_kernel_map(ctx->phys, ctx->size);

    if (uacpi_unlikely(ctx->virt == UACPI_NULL)) {
        ret = UACPI_STATUS_MAPPING_FAILED;
        uacpi_trace_region_error(data->region_node, "unable to map", ret);
        uacpi_kernel_free(ctx);
        return ret;
    }

    data->out_region_context = ctx;
    return ret;
}

static uacpi_status memory_region_detach(uacpi_region_detach_data *data)
{
    struct memory_region_ctx *ctx = data->region_context;

    uacpi_kernel_unmap(ctx->virt, ctx->size);
    uacpi_kernel_free(ctx);
    return UACPI_STATUS_OK;
}

struct io_region_ctx {
    uacpi_io_addr base;
    uacpi_handle handle;
};

static uacpi_status io_region_attach(uacpi_region_attach_data *data)
{
    struct io_region_ctx *ctx;
    uacpi_operation_region *op_region;
    uacpi_status ret;

    ctx = uacpi_kernel_alloc(sizeof(*ctx));
    if (ctx == UACPI_NULL)
        return UACPI_STATUS_OUT_OF_MEMORY;

    op_region = uacpi_namespace_node_get_object(data->region_node)->op_region;
    ctx->base = op_region->offset;

    ret = uacpi_kernel_io_map(ctx->base, op_region->length, &ctx->handle);
    if (uacpi_unlikely_error(ret)) {
        uacpi_trace_region_error(
            data->region_node, "unable to map an IO", ret
        );
        uacpi_kernel_free(ctx);
        return ret;
    }

    data->out_region_context = ctx;
    return ret;
}

static uacpi_status io_region_detach(uacpi_region_detach_data *data)
{
    struct io_region_ctx *ctx = data->region_context;

    uacpi_kernel_io_unmap(ctx->handle);
    uacpi_kernel_free(ctx);
    return UACPI_STATUS_OK;
}

static uacpi_status memory_read(void *ptr, uacpi_u8 width, uacpi_u64 *out)
{
    switch (width) {
    case 1:
        *out = *(volatile uacpi_u8*)ptr;
        break;
    case 2:
        *out = *(volatile uacpi_u16*)ptr;
        break;
    case 4:
        *out = *(volatile uacpi_u32*)ptr;
        break;
    case 8:
        *out = *(volatile uacpi_u64*)ptr;
        break;
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status memory_write(void *ptr, uacpi_u8 width, uacpi_u64 in)
{
    switch (width) {
    case 1:
        *(volatile uacpi_u8*)ptr = in;
        break;
    case 2:
        *(volatile uacpi_u16*)ptr = in;
        break;
    case 4:
        *(volatile uacpi_u32*)ptr = in;
        break;
    case 8:
        *(volatile uacpi_u64*)ptr = in;
        break;
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status memory_region_do_rw(
    uacpi_region_op op, uacpi_region_rw_data *data
)
{
    struct memory_region_ctx *ctx = data->region_context;
    uacpi_u8 *ptr;

    ptr = ctx->virt + (data->address - ctx->phys);

    return op == UACPI_REGION_OP_READ ?
        memory_read(ptr, data->byte_width, &data->value) :
        memory_write(ptr, data->byte_width, data->value);
}

static uacpi_status handle_memory_region(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op) {
    case UACPI_REGION_OP_ATTACH:
        return memory_region_attach(op_data);
    case UACPI_REGION_OP_DETACH:
        return memory_region_detach(op_data);
    case UACPI_REGION_OP_READ:
    case UACPI_REGION_OP_WRITE:
        return memory_region_do_rw(op, op_data);
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
}

static uacpi_status io_region_do_rw(
    uacpi_region_op op, uacpi_region_rw_data *data
)
{
    struct io_region_ctx *ctx = data->region_context;
    uacpi_u8 width;
    uacpi_size offset;

    offset = data->offset - ctx->base;
    width = data->byte_width;

    return op == UACPI_REGION_OP_READ ?
        uacpi_kernel_io_read(ctx->handle, offset, width, &data->value) :
        uacpi_kernel_io_write(ctx->handle, offset, width, data->value);
}

static uacpi_status handle_io_region(uacpi_region_op op, uacpi_handle op_data)
{
    switch (op) {
    case UACPI_REGION_OP_ATTACH:
        return io_region_attach(op_data);
    case UACPI_REGION_OP_DETACH:
        return io_region_detach(op_data);
    case UACPI_REGION_OP_READ:
    case UACPI_REGION_OP_WRITE:
        return io_region_do_rw(op, op_data);
    default:
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
}

void uacpi_install_default_address_space_handlers(void)
{
    uacpi_namespace_node *root;

    root = uacpi_namespace_root();

    uacpi_install_address_space_handler(
        root, UACPI_ADDRESS_SPACE_SYSTEM_MEMORY,
        handle_memory_region, UACPI_NULL
    );

    uacpi_install_address_space_handler(
        root, UACPI_ADDRESS_SPACE_SYSTEM_IO,
        handle_io_region, UACPI_NULL
    );

    uacpi_install_address_space_handler(
        root, UACPI_ADDRESS_SPACE_PCI_CONFIG,
        handle_pci_region, UACPI_NULL
    );
}
